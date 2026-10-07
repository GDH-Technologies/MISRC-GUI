#!/usr/bin/env python3
import argparse
import getpass
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def extract_function_body(source: str, signature: str) -> str:
    pattern = re.compile(rf"{re.escape(signature)}\s*\{{(?P<body>.*?)\n\}}", re.S)
    match = pattern.search(source)
    if not match:
        raise RuntimeError(f"Could not find function body for {signature}")
    return match.group("body")


GUI_WINDOW_CLASS_DEFINE = re.compile(r'^#define\s+MISRC_WINDOW_CLASS_TITLE\s+"([^"]+)"', re.M)

# Tokens that would bake a build version back into WM_CLASS. This is the exact
# regression that broke the dock icon: every build produced a different WM_CLASS,
# so a launcher written by one build stopped matching the next build's window and
# GNOME fell back to the generic executable icon.
WM_CLASS_VERSION_TOKENS = (
    "${BUILD_VERSION}",
    "$BUILD_VERSION",
    "${gui_version}",
    "$gui_version",
    "${VERSION}",
    "MIRSC_TOOLS_VERSION",
)


def strip_shell_comments(source: str) -> str:
    """Drop whole-line # comments so a comment may discuss what it forbids."""
    return "\n".join(l for l in source.splitlines() if not l.lstrip().startswith("#"))


def strip_c_comments(source: str) -> str:
    """Blank out /* */ and // comments, preserving line structure.

    String and character literals are copied through untouched. Without that a
    literal like "rtsp://" reads as the start of a line comment and silently
    blanks the rest of the line, so a guard searching for it finds nothing and
    reports the code missing when it is right there."""
    out = []
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in source[i:end]))
            i = end
        elif source.startswith("//", i):
            end = source.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif ch in ('"', "'"):
            # Copy the literal verbatim, honouring backslash escapes so that an
            # escaped quote does not end it early.
            out.append(ch)
            i += 1
            while i < n:
                if source[i] == "\\" and i + 1 < n:
                    out.append(source[i:i + 2])
                    i += 2
                    continue
                out.append(source[i])
                if source[i] == ch or source[i] == "\n":
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def read_gui_window_class_name(gui_c_path: Path) -> str:
    """The one source of truth for WM_CLASS. misrc_gui.c creates its window under
    this exact name, so every .desktop we generate must set StartupWMClass to it."""
    match = GUI_WINDOW_CLASS_DEFINE.search(read_text(gui_c_path))
    if not match:
        raise RuntimeError("Could not find #define MISRC_WINDOW_CLASS_TITLE in misrc_gui.c")
    return match.group(1)


def find_versioned_wm_class(text: str) -> List[str]:
    """Lines that decorate a WM_CLASS assignment with a build version."""
    offenders = []
    for line in text.splitlines():
        if "WMClass" not in line and "startup_wm_class" not in line:
            continue
        if any(token in line for token in WM_CLASS_VERSION_TOKENS):
            offenders.append(line.strip())
    return offenders


def extract_deploy_desktop_entry(deploy_text: str) -> str:
    """The .desktop body that selfhosted-deploy.yml installs onto workflow-master."""
    marker = 'cat > "$app_dir/misrc_gui.desktop" <<EOF'
    start = deploy_text.find(marker)
    if start < 0:
        raise RuntimeError("Could not find the installed .desktop heredoc in selfhosted-deploy.yml")
    start = deploy_text.find("\n", start)
    if start < 0:
        raise RuntimeError("Malformed .desktop heredoc in selfhosted-deploy.yml")
    start += 1
    end = deploy_text.find("\n          EOF", start)
    if end < 0:
        raise RuntimeError("Unterminated .desktop heredoc in selfhosted-deploy.yml")
    return textwrap.dedent(deploy_text[start:end]).lstrip("\n")


def run_checked(command: List[str], *, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, capture_output=True, text=True, env=env)


def check_cross_platform_workflow_coverage(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "linux-appimage:",
        "arch: x86_64",
        "arch: arm64",
        "windows-exe:",
        "runs-on: windows-2022",
        "macos-app-build:",
        "runner: macos-14",
        "runner: macos-15-intel",
        "macos-app-universal:",
        "android-apk:",
        "release:",
        "- linux-appimage",
        "- windows-exe",
        "- macos-app-universal",
        "- android-apk",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow cross-platform coverage is missing required snippet: {snippet}")
    return 0


def check_no_legacy_release_sanity_workflow(legacy_workflow_path: Path) -> int:
    if legacy_workflow_path.exists():
        return fail(f"Legacy workflow should be removed after replacement: {legacy_workflow_path}")
    return 0


def check_cross_platform_smoke_tests(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_smokes = [
        "\"$BUILD_DIR/misrc_gui\" --smoke-test",
        "APPIMAGE_EXTRACT_AND_RUN=1 \"./$APPIMAGE_NAME\" --smoke-test",
        "./dist/MISRC.exe --smoke-test",
        "dist/MISRC.app/Contents/MacOS/MISRC --smoke-test",
    ]
    for smoke in required_smokes:
        if smoke not in workflow_text:
            return fail(f"Missing expected smoke test command in workflow: {smoke}")
    return 0
def check_actions_runtime_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_action_pins = [
        "actions/checkout@v4",
        "actions/setup-python@v5",
        "actions/upload-artifact@v4",
        "actions/download-artifact@v4",
        "actions/cache@v4",
    ]
    for pin in forbidden_action_pins:
        if pin in workflow_text:
            return fail(f"Workflow contains deprecated action pin that triggers warning annotations: {pin}")

    required_action_pins = [
        "actions/checkout@v6",
        "actions/setup-python@v6",
        "actions/upload-artifact@v7",
        "actions/download-artifact@v8",
        "actions/cache@v6",
    ]
    for pin in required_action_pins:
        if pin not in workflow_text:
            return fail(f"Workflow is missing expected modern action pin: {pin}")
    return 0
def check_macos_brew_install_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_snippets = [
        "brew install cmake fftw flac libusb libuvc meson nasm ninja pkg-config libsoxr",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow contains non-conditional brew install that emits warning annotations: {snippet}")
    required_snippets = [
        "for formula in cmake fftw flac libusb libuvc meson nasm ninja pkgconf libsoxr; do",
        "if ! brew list --versions \"$formula\" >/dev/null 2>&1; then",
        "brew install \"$formula\"",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing macOS conditional brew install snippet: {snippet}")
    return 0
def check_msys2_toolchain_policy(workflow_path: Path) -> int:
    """MSYS2 toolchain policy for the Windows CI jobs.

    - MINGW64 is deprecated by MSYS2 (run 37035393223 emitted the
      '[msystem-mingw64] MINGW64 is deprecated. Migrate to UCRT64 or CLANG64'
      warning annotation); the x86_64 job must use UCRT64.
    - Both Windows jobs must install libsoxr: run 37035393223's arm64 job
      logged 'libsoxr not found, building without resample support' because
      mingw-w64-clang-aarch64-libsoxr was missing from its install list, so
      the ARM64 build silently shipped without resample support while
      x86_64 had it.
    - The arm64 job must install the LLVM OpenMP runtime: MSYS2's clang-built
      static libsoxr.a references libomp (run 37053411644: ld.lld 'undefined
      symbol: __kmpc_fork_call / omp_init_lock' referenced by
      libsoxr.a(soxr.c.obj)/(filter.c.obj)); meson links -lomp for it.
    - The Windows x86_64 deps cache key must name the ucrt64 toolchain so a
      MINGW64-built .deps/install (msvcrt-based static libs) can never be
      reused by the UCRT64 job.
    """
    workflow_text = read_text(workflow_path)
    forbidden_snippets = [
        "msystem: MINGW64",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow uses the deprecated MSYS2 environment (migrate to UCRT64/CLANG64): {snippet}")
    required_snippets = [
        "msystem: UCRT64",
        "msystem: CLANGARM64",
        "mingw-w64-ucrt-x86_64-libsoxr",
        "mingw-w64-clang-aarch64-libsoxr",
        "mingw-w64-clang-aarch64-llvm-openmp",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required MSYS2 toolchain/resample parity snippet: {snippet}")
    if "deps-windows-ucrt64-x86_64-" not in workflow_text:
        return fail(
            "Windows x86_64 deps cache key must name the ucrt64 toolchain "
            "(deps-windows-ucrt64-x86_64-) so a MINGW64-built cache entry "
            "(msvcrt-based static libs) can never be reused after the "
            "UCRT64 migration"
        )
    return 0


def check_workflow_fft_dependency_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "libfftw3-dev",
        "mingw-w64-ucrt-x86_64-fftw",
        "mingw-w64-clang-aarch64-fftw",
        "for formula in cmake fftw flac libusb libuvc meson nasm ninja pkgconf libsoxr; do",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required FFT dependency snippet: {snippet}")

    fft_probe = "pkg-config --modversion fftw3f"
    fft_probe_count = workflow_text.count(fft_probe)
    if fft_probe_count != 4:
        return fail(f"Workflow must probe fftw3f exactly 4 times (linux/windows x86/windows arm64/macos), found {fft_probe_count}")
    return 0


# ---------------------------------------------------------------------------
# Libc direct-include contract (include-what-you-use, libc subset).
#
# Regression this prevents (Actions run 37035393223): gui_record_direct.c
# called malloc/free with no <stdlib.h> of its own. common/threading.h
# includes <stdlib.h> only in its POSIX branch, so glibc builds got malloc
# declared transitively while the MSYS2 (Windows) builds did not -> implicit
# declaration: a hard error on MSYS2 GCC/clang, only a warning on
# ubuntu-22.04 gcc 11 / Apple clang / NDK clang. The bug class is invisible
# on every platform except the one it breaks. This static guard makes the
# 10-second preflight (any platform, before any deps build) fail instead:
# a .c/.h whose TOP-LEVEL code (outside any platform-conditional #if, so
# compiled on every platform) calls a libc function must include the libc
# header itself, or get it from a project header that includes it OUTSIDE
# any platform-conditional #if. Platform-conditional provision (threading.h's
# POSIX-only <stdlib.h>) does NOT count - that is exactly the provision
# that compiles on glibc and breaks on MinGW. Calls inside platform-gated
# regions are exempt: by construction their includes live in the same branch
# (the threading.h/posix-branch pattern), and each platform's CI build
# verifies those branches compile with their own branch-local includes.
# ---------------------------------------------------------------------------

_PLATFORM_MACRO_RE = re.compile(
    r"\b(_WIN32|_WIN64|__CYGWIN__|__MINGW32__|__MINGW64__|MSVC|_MSC_VER|"
    r"__APPLE__|__MACH__|__ANDROID__|__linux__|__unix__|__FreeBSD__|"
    r"__NetBSD__|__OpenBSD__|__sun|__EMSCRIPTEN__)\b"
)

_LIBC_HEADER_FAMILIES = {
    "stdlib.h": (
        "malloc", "calloc", "realloc", "free", "exit", "abort", "atexit",
        "atoi", "atol", "strtol", "strtoul", "strtoll", "strtod", "strtof",
        "qsort", "bsearch", "abs", "labs", "rand", "srand", "getenv",
        "posix_memalign", "aligned_alloc",
    ),
    "stdio.h": (
        "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
        "vsnprintf", "fopen", "fclose", "fread", "fwrite", "fseek", "ftell",
        "feof", "ferror", "clearerr", "fflush", "rewind", "remove", "rename",
        "fputs", "fgets", "fgetc", "fputc", "putchar", "puts", "scanf",
        "sscanf", "getline", "perror", "fileno", "setvbuf", "ungetc",
    ),
    "string.h": (
        "memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp",
        "strncmp", "strcpy", "strncpy", "strcat", "strncat", "strchr",
        "strrchr", "strstr", "strdup", "strndup", "strtok", "strtok_r",
        "strcasecmp", "strncasecmp", "strerror", "strcspn", "strspn",
        "strnlen",
    ),
    "time.h": (
        "clock_gettime", "nanosleep", "localtime", "gmtime", "strftime",
        "mktime", "difftime",
    ),
    "math.h": (
        "sin", "cos", "tan", "sqrt", "pow", "floor", "ceil", "fabs",
        "fmod", "round", "lround", "atan2", "exp", "log10", "asin",
        "acos", "atan", "hypot", "fmin", "fmax", "trunc", "rint",
    ),
}


def _strip_c_lines(text: str) -> List[str]:
    strip = _make_c_stripper()
    return [strip(line) for line in text.splitlines()]


def _parse_source(path: Path) -> Tuple[set, List[Path], str]:
    """Parse one source file for the libc direct-include contract.

    Returns (unconditional_system_includes, unconditional_project_include_paths,
    top_level_text) where top_level_text is the comment/string-stripped text
    of the lines OUTSIDE any platform-conditional #if region (code compiled on
    every platform).

    - Directive recognition uses comment/string-stripped lines (a directive
      inside a comment strips to nothing and is skipped), but include names
      are extracted from the RAW line: the stripper blanks string literals,
      which would destroy `#include "header.h"` names.
    - An #include counts as unconditional only if no enclosing #if/#ifdef/#elif
      condition mentions a platform macro. Include guards (#ifndef X_H) do not
      mention platform macros, so guarded headers still count. threading.h's
      POSIX-only <stdlib.h> sits inside `#else` of a _WIN32 chain and does not
      count. Unresolvable project includes (e.g. generated version.h) are
      dropped.
    """
    text = read_text(path)
    raw_lines = text.splitlines()
    stripped_lines = _strip_c_lines(text)
    system: set = set()
    project: List[Path] = []
    top_level_lines: List[str] = []
    stack: List[bool] = []
    for raw_line, stripped_line in zip(raw_lines, stripped_lines):
        s = stripped_line.strip()
        m = re.match(r"#\s*(\w+)", s)
        m_raw = re.match(r"#\s*\w+\s*(.*)", raw_line.strip())
        rest = m_raw.group(1).strip() if m_raw else ""
        if not m:
            if not any(stack):
                top_level_lines.append(stripped_line)
            continue
        if not any(stack):
            top_level_lines.append(s)
        directive = m.group(1)
        if directive in ("if", "ifdef", "ifndef"):
            stack.append(bool(_PLATFORM_MACRO_RE.search(rest)))
        elif directive == "elif" and stack:
            stack[-1] = stack[-1] or bool(_PLATFORM_MACRO_RE.search(rest))
        elif directive == "endif":
            if stack:
                stack.pop()
        elif directive == "include" and not any(stack):
            inc_sys = re.match(r"<([^>]+)>", rest)
            inc_proj = re.match(r'"([^"]+)"', rest)
            if inc_sys:
                system.add(inc_sys.group(1))
            elif inc_proj:
                cand = path.parent / inc_proj.group(1)
                if cand.exists():
                    project.append(cand)
    return system, project, "\n".join(top_level_lines)


def check_libc_direct_include_contract(repo_root: Path) -> int:
    root = repo_root / "misrc_tools"
    if not root.exists():
        return 0
    parsed: Dict[Path, Tuple[set, List[Path], str]] = {}

    def parse(path: Path) -> Tuple[set, List[Path], str]:
        if path not in parsed:
            parsed[path] = _parse_source(path)
        return parsed[path]

    def available_headers(entry: Path) -> set:
        seen = {entry}
        stack = [entry]
        available = set()
        while stack:
            path = stack.pop()
            system, project_paths, _ = parse(path)
            available |= system
            for q in project_paths:
                if q not in seen:
                    seen.add(q)
                    stack.append(q)
        return available

    failures: List[str] = []
    files = sorted(
        p for p in root.rglob("*")
        if p.suffix in (".c", ".h") and p.is_file()
        and ".deps" not in p.parts and "build" not in p.parts
    )
    for path in files:
        _, _, top_level_text = parse(path)
        if not top_level_text:
            continue
        available = available_headers(path)
        for header, funcs in _LIBC_HEADER_FAMILIES.items():
            if header in available:
                continue
            called = [
                fn for fn in funcs
                if re.search(rf"(?<![\w.>]){re.escape(fn)}\s*\(", top_level_text)
            ]
            if called:
                rel = path.relative_to(repo_root)
                failures.append(
                    f"{rel}: top-level code calls {', '.join(called)} without {header} "
                    f"(direct or via a non-platform-conditional project header)"
                )
    if failures:
        for line in failures:
            print(f"ERROR: libc direct-include violation: {line}", file=sys.stderr)
        return fail(
            "libc direct-include contract violated in "
            f"{len(failures)} file(s); add the missing #include(s). A libc call "
            "satisfied only via a platform-conditional transitive include "
            "(e.g. malloc via threading.h's POSIX-only <stdlib.h>) compiles on "
            "glibc but is an implicit-declaration hard error on MSYS2 "
            "(Windows) - run 37035393223."
        )
    return 0


def check_meson_fft_policy(meson_path: Path) -> int:
    meson_text = read_text(meson_path)
    required_snippets = [
        "error('FFTW3 (fftw3f) is required for misrc_gui.",
        "gui_deps = deps + [ raylib_dep, fftw3f_dep ]",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing required FFT policy snippet: {snippet}")

    forbidden_snippets = [
        "message('FFTW3 not found, building without FFT support')",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains forbidden optional-FFT fallback snippet: {snippet}")
    return 0


def check_meson_vendored_hsdaoh_policy(meson_path: Path) -> int:
    """Ensure meson.build prefers the vendored .deps/install hsdaoh (mirrors CI)
    so a bare local build cannot silently link a stale system libhsdaoh that
    lacks the v1.0.9 connect fixes."""
    meson_text = read_text(meson_path)
    required_snippets = [
        # The vendored-pc probe was generalized (lib/lib64, hsdaoh/libhsdaoh)
        # in the Fedora-build merge; these match the probing implementation.
        "vendored_pc_dir",
        "fs.exists(hsdaoh_deps_root / _libdir / 'pkgconfig' / (_pc + '.pc'))",
        "Using vendored hsdaoh from .deps/install (mirrors CI",
        "declare_dependency",
        "deps = [ hsdaoh_dep ]",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing vendored-hsdaoh policy snippet: {snippet}")
    forbidden_snippets = [
        "deps = [ dependency('hsdaoh', static: windows_static_deps) ]",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains bare system-hsdaoh dependency (no vendored guard): {snippet}")
    return 0


def check_built_gui_links_vendored_hsdaoh(repo_root: Path, gui_path: Optional[Path] = None) -> int:
    """Runtime check: assert the built misrc_gui links the vendored hsdaoh from
    .deps/install, not a stale system libhsdaoh. Platform-aware:
      - Linux: ldd (dynamic, must resolve to .deps/install)
      - macOS: otool -L (dynamic, must resolve to @rpath/.deps/install)
      - Windows (MSYS2/MinGW static build): no runtime hsdaoh dylib expected —
        assert objdump -p shows no libhsdaoh DLL NEEDED (it's statically linked).
    In pre-build (preflight) mode with no gui_path and no default build/misrc_gui,
    this skips (returns 0) so preflight still passes; CI runs it again post-build
    against the real $BUILD_DIR/misrc_gui via --gui-path.
    """
    gui = gui_path if gui_path is not None else (repo_root / "misrc_tools" / "build" / "misrc_gui")
    if not gui.exists():
        return 0  # no local build present; preflight or no-build context

    if sys.platform == "darwin":
        try:
            res = run_checked(["otool", "-L", str(gui)])
        except subprocess.CalledProcessError as exc:
            return fail(f"otool -L misrc_gui failed (rc={exc.returncode}): {(exc.stderr or '').strip()}")
        found_hsdaoh = False
        for line in res.stdout.splitlines():
            if "libhsdaoh" in line:
                found_hsdaoh = True
                stripped = line.strip()
                # Portable macOS bundles use @rpath/libhsdaoh... resolved from
                # the app Frameworks dir; a bare /usr/local/lib or /opt/homebrew
                # path means a stale system lib got linked.
                if "/usr/local/lib/" in line or "/opt/homebrew/" in line or "/usr/lib/" in line:
                    return fail(f"misrc_gui links a SYSTEM hsdaoh on macOS (expected @rpath/.deps/install): {stripped}")
        if not found_hsdaoh:
            return fail("misrc_gui does not link libhsdaoh at all (macOS otool -L)")
        return 0

    if os.name == "nt" or sys.platform.startswith("win"):
        # Windows MSYS2/MinGW build statically links hsdaoh (.deps/install/lib/libhsdaoh.a).
        # A correctly-built misrc_gui.exe must NOT have a libhsdaoh DLL NEEDED entry.
        objdump = shutil.which("objdump")
        if not objdump:
            return 0  # objdump unavailable (non-MSYS2 python); skip on Windows
        res = subprocess.run([objdump, "-p", str(gui)], capture_output=True, text=True)
        if res.returncode != 0:
            return fail(f"objdump -p misrc_gui failed: {res.stderr.strip()}")
        for line in res.stdout.splitlines():
            if "DLL Name:" in line and "hsdaoh" in line.lower():
                return fail(f"misrc_gui.exe has a runtime libhsdaoh DLL dependency (expected static link to vendored .deps/install/lib/libhsdaoh.a): {line.strip()}")
        return 0  # static: no hsdaoh DLL NEEDED -> correct

    # Linux (and other ELF platforms): ldd
    try:
        res = run_checked(["ldd", str(gui)])
    except subprocess.CalledProcessError as exc:
        return fail(f"ldd misrc_gui failed (rc={exc.returncode}): {(exc.stderr or '').strip()} — not a dynamic ELF binary?")
    found_hsdaoh = False
    for line in res.stdout.splitlines():
        if "libhsdaoh" in line:
            found_hsdaoh = True
            stripped = line.strip()
            if "/usr/local/lib/" in line or " /usr/lib/" in line or " /lib/" in line:
                return fail(f"misrc_gui links a SYSTEM hsdaoh (stale, lacks v1.0.9 connect fixes); expected vendored .deps/install: {stripped}")
            if ".deps/install" not in line:
                return fail(f"misrc_gui links hsdaoh from unexpected path (expected .deps/install): {stripped}")
    if not found_hsdaoh:
        return fail("misrc_gui does not link libhsdaoh at all")
    return 0


def check_meson_fx3_policy(meson_path: Path) -> int:
    """Ensure meson.build builds the vendored cyusb compatibility shim from
    source (third_party/cyusb/cyusb.c) on all platforms and sets ENABLE_FX3=1
    when libusb-1.0 is present. This makes FX3 native on every platform with no
    external libcyusb package and no system-lib shadowing possible."""
    meson_text = read_text(meson_path)
    required_snippets = [
        "third_party/cyusb/cyusb.c",
        "static_library('cyusb_compat'",
        "declare_dependency",
        "-DENABLE_FX3=1",
        "fx3_enabled = true",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"meson.build is missing FX3 native-build policy snippet: {snippet}")
    forbidden_snippets = [
        "dependency('libcyusb', required : false)",
    ]
    for snippet in forbidden_snippets:
        if snippet in meson_text:
            return fail(f"meson.build still contains bare external libcyusb pkg-config lookup (no vendored shim): {snippet}")
    return 0


def check_built_gui_has_fx3_symbols(repo_root: Path, gui_path: Optional[Path] = None) -> int:
    """Runtime check: the built misrc_gui must have FX3 compiled in. Catches a
    silent FX3-disable where libusb was missing and FX3 compiled out, which would
    ship a binary without FX3 support and nobody would know. Uses nm/strings."""
    gui = gui_path if gui_path is not None else (repo_root / "misrc_tools" / "build" / "misrc_gui")
    if not gui.exists():
        return 0  # no local build present; preflight skips
    nm = shutil.which("nm")
    if nm:
        res = subprocess.run([nm, str(gui)], capture_output=True, text=True)
        if res.returncode == 0 and "gui_fx3_start" in res.stdout:
            return 0
    # Fall back to strings (works on stripped binaries + Windows .exe)
    strings = shutil.which("strings")
    if not strings:
        return fail("neither nm nor strings available to verify FX3 symbols in misrc_gui")
    res = subprocess.run([strings, str(gui)], capture_output=True, text=True)
    if res.returncode != 0:
        return fail(f"strings misrc_gui failed: {res.stderr.strip()}")
    # gui_fx3_* log strings are present when gui_fx3.c is compiled in.
    if "[FX3]" not in res.stdout or "fx3usbadc start command sent" not in res.stdout:
        return fail("misrc_gui has no FX3 symbols/strings — FX3 did not compile in (libusb missing or ENABLE_FX3 not set)")
    return 0


APPIMAGE_ASSET_DIR = Path("assets/appimage")
APPIMAGE_DESKTOP_ID = "misrc_gui"
# X11 WM_CLASS class the GUI window is created with; the launcher's
# StartupWMClass must equal it and must NOT contain the version.
APPIMAGE_WM_CLASS = "MISRC Capture"


def read_desktop_key(text: str, key: str) -> Optional[str]:
    for line in text.splitlines():
        if line.startswith(f"{key}="):
            return line.split("=", 1)[1]
    return None


def check_linux_desktop_metadata(repo_root: Path, workflow_path: Path) -> int:
    """The AppDir desktop entry and AppRun are real repo files shared by the CI
    workflow and scripts/build-appimage-local.sh (inline heredocs in YAML broke
    before). The desktop entry carries a version-independent StartupWMClass."""
    workflow_text = read_text(workflow_path)
    required_workflow = [
        "install -m 0755 assets/appimage/AppRun AppDir/AppRun",
        "install -m 0644 assets/appimage/misrc_gui.desktop AppDir/misrc_gui.desktop",
        "-d AppDir/misrc_gui.desktop",
        "ln -sf misrc.png AppDir/.DirIcon",
    ]
    for snippet in required_workflow:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing Linux desktop integration snippet: {snippet}")
    forbidden_workflow = [
        "cat > AppDir/AppRun",
        "cat > AppDir/misrc.desktop",
        "MISRC Capture ${BUILD_VERSION}",
    ]
    for snippet in forbidden_workflow:
        if snippet in workflow_text:
            return fail(
                "Workflow still contains a forbidden inline/versioned AppImage desktop snippet "
                f"(use assets/appimage/* and a version-independent StartupWMClass): {snippet}"
            )

    local_script = read_text(repo_root / "scripts" / "build-appimage-local.sh")
    for snippet in ('assets/appimage/AppRun', 'assets/appimage/misrc_gui.desktop'):
        if snippet not in local_script:
            return fail(f"scripts/build-appimage-local.sh must use the shared file: {snippet}")
    for snippet in ('cat > "$appdir/AppRun"', 'cat > "$appdir/misrc.desktop"'):
        if snippet in local_script:
            return fail(f"scripts/build-appimage-local.sh still has an inline copy: {snippet}")

    desktop_path = repo_root / APPIMAGE_ASSET_DIR / "misrc_gui.desktop"
    if not desktop_path.exists():
        return fail(f"Missing AppDir desktop entry: {desktop_path}")
    desktop_text = read_text(desktop_path)
    expected = {
        "Type": "Application",
        "Exec": "misrc_gui",
        "Icon": "misrc",
        "Terminal": "false",
        "StartupNotify": "true",
        "StartupWMClass": APPIMAGE_WM_CLASS,
    }
    for key, value in expected.items():
        actual = read_desktop_key(desktop_text, key)
        if actual != value:
            return fail(f"{desktop_path.name}: expected {key}={value!r}, found {actual!r}")
    if not (repo_root / APPIMAGE_ASSET_DIR / "AppRun").exists():
        return fail("Missing assets/appimage/AppRun")
    return 0


def check_window_identity_contract(repo_root: Path) -> int:
    """GLFW (raylib) builds X11 WM_CLASS from the window title at creation, so
    the window must be created with a fixed title (the versioned title changed
    the class every release and left StartupWMClass stale). The fixed title,
    the launcher StartupWMClass and AppRun's launcher text must all agree."""
    gui_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    define = f'#define MISRC_WINDOW_CLASS_TITLE "{APPIMAGE_WM_CLASS}"'
    required = [
        define,
        f'setenv("RESOURCE_NAME", "{APPIMAGE_DESKTOP_ID}", 0);',
        "InitWindow(default_window_width, default_window_height, MISRC_WINDOW_CLASS_TITLE);",
        "SetWindowTitle(window_title);",
    ]
    for snippet in required:
        if snippet not in gui_c:
            return fail(f"misrc_gui.c is missing window identity snippet: {snippet}")
    if "InitWindow(default_window_width, default_window_height, window_title)" in gui_c:
        return fail("misrc_gui.c must not create the window with the versioned title (unstable WM_CLASS)")
    init_pos = gui_c.find("InitWindow(default_window_width")
    title_pos = gui_c.find("SetWindowTitle(window_title);")
    if init_pos < 0 or title_pos < init_pos:
        return fail("SetWindowTitle(window_title) must come after InitWindow()")

    apprun = read_text(repo_root / APPIMAGE_ASSET_DIR / "AppRun")
    if f'export RESOURCE_NAME="{APPIMAGE_DESKTOP_ID}"' not in apprun:
        return fail(f'AppRun must export RESOURCE_NAME="{APPIMAGE_DESKTOP_ID}"')
    if f"StartupWMClass={APPIMAGE_WM_CLASS}" not in apprun:
        return fail(f"AppRun launcher text must use StartupWMClass={APPIMAGE_WM_CLASS}")
    if f'launcher="${{data_home}}/applications/{APPIMAGE_DESKTOP_ID}.desktop"' not in apprun:
        return fail(f"AppRun must install the launcher as {APPIMAGE_DESKTOP_ID}.desktop")
    return 0


def check_macos_layout_policy(gui_ui_c_path: Path) -> int:
    source = read_text(gui_ui_c_path)
    width_body = extract_function_body(source, "static int gui_ui_get_base_layout_width(void)")
    height_body = extract_function_body(source, "static int gui_ui_get_base_layout_height(void)")

    if "#if defined(__APPLE__)" not in width_body:
        return fail("gui_ui_get_base_layout_width() is missing __APPLE__ guard")
    if "#if defined(__APPLE__)" not in height_body:
        return fail("gui_ui_get_base_layout_height() is missing __APPLE__ guard")
    if "GetScreenWidth();" not in width_body:
        return fail("gui_ui_get_base_layout_width() must use GetScreenWidth() on macOS")
    if "GetScreenHeight();" not in height_body:
        return fail("gui_ui_get_base_layout_height() must use GetScreenHeight() on macOS")
    if "GetRenderWidth();" not in width_body:
        return fail("gui_ui_get_base_layout_width() must use GetRenderWidth() for non-macOS")
    if "GetRenderHeight();" not in height_body:
        return fail("gui_ui_get_base_layout_height() must use GetRenderHeight() for non-macOS")
    return 0

def check_macos_admin_elevation_contract(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    required_snippets = [
        "static int gui_macos_relaunch_as_admin_if_needed(int argc, char **argv)",
        "MISRC_GUI_ELEVATED",
        "do shell script (item 1 of argv) with administrator privileges",
        "int elevate_rc = gui_macos_relaunch_as_admin_if_needed(argc, argv);",
        "Administrator permissions are required for MS2130 hsdaoh/libusb capture.",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing required macOS startup elevation contract snippet in misrc_gui.c: {snippet}")
    return 0


def check_windows_meson_subsystem_contract(meson_path: Path) -> int:
    meson_text = read_text(meson_path)
    required_snippets = [
        "gui_win_subsystem = 'console'",
        "gui_win_subsystem = 'windows'",
        "win_subsystem: gui_win_subsystem",
    ]
    for snippet in required_snippets:
        if snippet not in meson_text:
            return fail(f"Missing Windows GUI subsystem contract snippet in meson.build: {snippet}")
    return 0


def check_dev_version_naming(repo_root: Path, meson_path: Path, workflow_path: Path) -> int:
    """Dev/untagged builds MUST use a date-stamped version (dev-YYYY-MM-DD-<sha>)
    derived by misrc_tools/git-version.sh, not a hardcoded "vX.Y.Z-dev" literal.
    Regression: the repo carried a hardcoded vN.N.N-dev literal in git-version.sh
    + 4 CI fallbacks and another in the VERSION file while the current release
    had advanced past it, so dev builds reported a version behind the last
    release. This guard forbids stale vX.Y.Z-dev literals across the
    version-resolution path and requires git-version.sh to derive the dev string
    from the current UTC date.
    ci_guard_tests.py check_dev_version_naming."""
    stale_re = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+-dev")
    targets = [
        meson_path,
        workflow_path,
        repo_root / "misrc_tools" / "git-version.sh",
        repo_root / "misrc_tools" / "ci-resolve-version.sh",
        repo_root / "android" / "build-apk.sh",
        repo_root / "scripts" / "build-appimage-local.sh",
        repo_root / "VERSION",
    ]
    for path in targets:
        if not path.exists():
            continue
        m = stale_re.search(read_text(path))
        if m:
            rel = path.relative_to(repo_root)
            return fail(
                f"{rel} contains a stale hardcoded dev-version literal ({m.group(0)}). "
                "Dev versions must be date-stamped (dev-YYYY-MM-DD-<sha>) via "
                "misrc_tools/git-version.sh, not a vX.Y.Z-dev string that goes "
                "stale as releases advance."
            )
    gv = repo_root / "misrc_tools" / "git-version.sh"
    if gv.exists():
        gv_text = read_text(gv)
        if "date -u" not in gv_text or "dev-" not in gv_text:
            return fail(
                "misrc_tools/git-version.sh must derive the untagged dev version "
                "from `date -u` with a `dev-` prefix (date-stamped scheme)."
            )
        if "--ignore-cr-at-eol" not in gv_text:
            return fail(
                "misrc_tools/git-version.sh dirty check must use --ignore-cr-at-eol "
                "so a fresh Windows checkout (CRLF) does not phantom-tag -dirty "
                "under MSYS2 git (autocrlf=false). It is a no-op on Linux/macOS."
            )
    return 0


def check_no_tracked_generated_dirty_sources(repo_root: Path) -> int:
    """Generated, machine-specific files that `git-version.sh`'s dirty check
    would see MUST be gitignored (untracked), not committed. Regression:
    android/deps-versions.txt was tracked and overwritten by
    build-deps-android.sh with a build timestamp + host NDK path, so every
    Android CI build dirty-tagged the version. Same class as the gitignored
    android/aarch64-linux-android.ini cross-file. Asserts both stay untracked."""
    targets = [
        repo_root / "android" / "deps-versions.txt",
        repo_root / "android" / "aarch64-linux-android.ini",
    ]
    for path in targets:
        if not path.exists():
            continue
        rel = path.relative_to(repo_root)
        tracked = subprocess.run(
            ["git", "ls-files", "--error-unmatch", str(path)],
            cwd=repo_root, capture_output=True, text=True,
        )
        if tracked.returncode == 0:
            return fail(
                f"{rel} is tracked but is a generated, machine-specific file. "
                "It dirty-tags the version string on any host/CI that regenerates "
                "it. Add it to .gitignore and `git rm --cached` it."
            )
        ign = subprocess.run(
            ["git", "check-ignore", str(path)],
            cwd=repo_root, capture_output=True, text=True,
        )
        if ign.returncode != 0:
            return fail(
                f"{rel} is untracked but NOT gitignored; add it to .gitignore so "
                "regenerating it cannot dirty the tree."
            )
    return 0


# Optional-dependency feature macros defined by misrc_tools/meson.build. A
# struct member declared inside an enabled #if <MACRO> branch must not be
# referenced (->name / .name) outside that branch, or the build breaks when
# the dependency is absent. Regression 64b2171: gui_demod.c declared
# soxr_in_buf/soxr_out_buf inside #if LIBSOXR_ENABLED but referenced them
# unguarded; Windows arm64 (no mingw-w64-clang-aarch64-libsoxr =>
# LIBSOXR_ENABLED=0) failed to compile with "no member named 'soxr_in_buf'".
_OPTIONAL_DEP_MACROS = (
    "LIBSOXR_ENABLED",
    "LIBFLAC_ENABLED",
    "LIBFFTW_ENABLED",
    "LIBASOUND_ENABLED",
    "ENABLE_FX3",
    "ENABLE_DDD",
    "ENABLE_RTLSDR",
)

_C_TYPE_KEYWORDS = {
    "const", "static", "extern", "volatile", "register", "auto",
    "unsigned", "signed", "short", "long", "int", "char", "float",
    "double", "void", "bool", "struct", "union", "enum",
    "size_t", "ssize_t", "ptrdiff_t", "wchar_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
}


def _make_c_stripper():
    """Return a stateful function that strips C comments and string/char
    literals from successive lines, replacing their contents with empty
    strings so braces/parens inside them do not affect structural parsing."""
    state = {"in_block_comment": False}

    def strip(line: str) -> str:
        out: List[str] = []
        i = 0
        n = len(line)
        while i < n:
            ch = line[i]
            nxt = line[i + 1] if i + 1 < n else ""
            if state["in_block_comment"]:
                if ch == "*" and nxt == "/":
                    state["in_block_comment"] = False
                    i += 2
                    continue
                i += 1
                continue
            if ch == "/" and nxt == "/":
                break  # line comment to end of line
            if ch == "/" and nxt == "*":
                state["in_block_comment"] = True
                i += 2
                continue
            if ch == '"':
                out.append('""')
                i += 1
                while i < n and line[i] != '"':
                    if line[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1  # skip closing quote
                continue
            if ch == "'":
                out.append("''")
                i += 1
                while i < n and line[i] != "'":
                    if line[i] == "\\" and i + 1 < n:
                        i += 2
                        continue
                    i += 1
                i += 1
                continue
            out.append(ch)
            i += 1
        return "".join(out)

    return strip


def _find_unguarded_struct_member_refs(source: str, macro: str) -> List[Tuple[int, str]]:
    """Return [(lineno, member_name), ...] for struct member references
    (->name / .name) that occur in plain code NOT enclosed by any conditional
    chain that mentions <macro>, while the member is declared only inside an
    enabled #if <macro> branch (not outside it, not in a #else branch). Those
    compile-fail when <macro> is undefined.

    Limitation: to avoid false positives on the legitimate #if/#elif !MACRO/
    #else idiom (where the final #else is only reached when MACRO is defined),
    a reference is NOT flagged if any enclosing #if/#elif/#else chain mentions
    <macro> at all. This means references inside a negated guard (#if !MACRO)
    are not caught — that rarer pattern would need a real preprocessor to
    evaluate precisely. The guard still catches the common regression where a
    guarded-only member is referenced in plain code with no macro conditional
    around it (regression 64b2171 in gui_demod.c)."""
    strip = _make_c_stripper()
    lines = [strip(l) for l in source.splitlines()]

    # pp_stack entries: (macro_or_None, in_else, chain_mentions_macro)
    pp_stack: List[Tuple[str, bool, bool]] = []
    in_struct = False
    struct_depth = 0
    struct_open_re = re.compile(r"(?:typedef\s+)?struct\s+\w*\s*\{")
    member_re = re.compile(r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;")
    ref_re = re.compile(r"(?:->|\.)\s*([A-Za-z_]\w*)")
    macro_token_re = re.compile(rf"\b{re.escape(macro)}\b")

    # member name -> per-region declared flags
    members: Dict[str, Dict[str, bool]] = {}
    # (lineno, name, region, enclosed_by_macro_chain)
    refs: List[Tuple[int, str, str, bool]] = []

    for idx, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith("#"):
            m = re.match(r"#\s*(\w+)(.*)", stripped)
            if m:
                directive, rest = m.group(1), m.group(2).strip()
                mentions = bool(macro_token_re.search(rest))
                if directive in ("if", "ifdef"):
                    negated = bool(re.search(rf"!\s*defined\s*\(\s*{re.escape(macro)}\s*\)", rest)) \
                        or bool(re.search(rf"!\s*{re.escape(macro)}\b", rest))
                    positive = mentions and not negated
                    pp_stack.append((macro if positive else None, False, mentions))
                elif directive == "ifndef":
                    # Block runs when <macro> is UNDEFINED; the chain mentions it.
                    pp_stack.append((None, False, mentions))
                elif directive == "elif":
                    if pp_stack:
                        top_macro, _, top_mentions = pp_stack[-1]
                        pp_stack[-1] = (top_macro, True, top_mentions or mentions)
                elif directive == "else":
                    if pp_stack:
                        top_macro, _, top_mentions = pp_stack[-1]
                        pp_stack[-1] = (top_macro, True, top_mentions)
                elif directive == "endif":
                    if pp_stack:
                        pp_stack.pop()
            continue

        # Region of this line relative to <macro>, and whether any enclosing
        # conditional chain mentions <macro> at all.
        region = "outside"
        enclosed = False
        for m_macro, m_else, m_mentions in pp_stack:
            if m_mentions:
                enclosed = True
            if m_macro == macro:
                region = "disabled" if m_else else "enabled"

        if in_struct:
            for ch in line:
                if ch == "{":
                    struct_depth += 1
                elif ch == "}":
                    struct_depth -= 1
            if struct_depth <= 0:
                in_struct = False
                continue
            for m in member_re.finditer(line):
                name = m.group(1)
                if name in _C_TYPE_KEYWORDS:
                    continue
                rec = members.setdefault(name, {"enabled": False, "disabled": False, "outside": False})
                rec[region] = True
            continue

        if struct_open_re.search(line):
            in_struct = True
            struct_depth = 0
            for ch in line:
                if ch == "{":
                    struct_depth += 1
                elif ch == "}":
                    struct_depth -= 1
            if struct_depth <= 0:
                in_struct = False
            continue

        # Outside any struct body: collect member-style references.
        for m in ref_re.finditer(line):
            refs.append((idx, m.group(1), region, enclosed))

    guarded_only = {
        name for name, rec in members.items()
        if rec["enabled"] and not rec["outside"] and not rec["disabled"]
    }
    return [(lineno, name) for lineno, name, region, enclosed in refs
            if name in guarded_only and region != "enabled" and not enclosed]


def check_windows_gui_link_no_dll_import_libs(meson_path: Path) -> int:
    """Forbid -l:lib<name>.dll.a DLL import-library references in meson.build.
    Windows builds are static (-static -static-libgcc); such import libs are
    only shipped by the matching MSYS2 mingw package, which the CI install
    list does not necessarily include. Regression 4cb9ced added
    -l:libglfw3.dll.a, breaking the Windows x86_64 link because raylib 5.5
    bundles GLFW into libraylib.a (no glfw DLL import lib is installed)."""
    meson_text = read_text(meson_path)
    # Strip meson line comments so the explanatory comment that names the
    # forbidden flag does not trip the check.
    code = "\n".join(line.split("#", 1)[0] for line in meson_text.splitlines())
    m = re.search(r"-l:lib\w+\.dll\.a", code)
    if m:
        return fail(
            "meson.build Windows GUI link flags reference a DLL import library ("
            + m.group(0) + "). Windows builds are static and the CI MSYS2 install "
            "list does not ship every mingw DLL import lib (regression 4cb9ced: "
            "-l:libglfw3.dll.a broke the link; raylib 5.5 bundles GLFW into "
            "libraylib.a). Link the static archive / system lib name instead."
        )
    return 0


def check_optional_dep_guard_consistency(repo_root: Path) -> int:
    """For each optional-dependency feature macro, scan the C sources for
    struct members declared inside an enabled #if <MACRO> branch that are
    referenced (->name / .name) outside the branch. Those fail to compile
    when the dependency is absent. Regression 64b2171: gui_demod.c soxr
    scratch buffers were #if LIBSOXR_ENABLED-guarded but referenced unguarded,
    breaking the Windows arm64 build (no libsoxr => LIBSOXR_ENABLED=0)."""
    tools_dir = repo_root / "misrc_tools"
    sources = sorted(tools_dir.rglob("*.c"))
    any_problem = False
    for src_path in sources:
        text = read_text(src_path)
        for macro in _OPTIONAL_DEP_MACROS:
            if macro not in text:
                continue
            for lineno, name in _find_unguarded_struct_member_refs(text, macro):
                any_problem = True
                rel = src_path.relative_to(repo_root)
                print(
                    f"ERROR: {rel}:{lineno}: struct member '{name}' is declared "
                    f"inside #if {macro} but referenced outside it; the build "
                    f"breaks when {macro} is undefined (a platform without the "
                    f"dependency). Move the member outside the guard or guard the "
                    f"reference.",
                    file=sys.stderr,
                )
    return 1 if any_problem else 0


def check_debug_view_contract(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    required_snippets = [
        "--debug-view",
        "bool debug_view = false;",
        "if (strcmp(argv[i], \"--debug-view\") == 0)",
        "gui_enable_debug_console();",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing debug-view runtime contract snippet in misrc_gui.c: {snippet}")
    return 0


def check_settings_persistence_contract(gui_settings_c_path: Path) -> int:
    source = read_text(gui_settings_c_path)
    required_snippets = [
        "gui_settings_ensure_parent_dirs(path)",
        "getenv(\"APPDATA\")",
        "getenv(\"LOCALAPPDATA\")",
        "getenv(\"XDG_CONFIG_HOME\")",
        "_mkdir(path)",
        "mkdir(path, 0700)",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing settings persistence contract snippet in gui_settings.c: {snippet}")

    save_pos = source.find("void gui_settings_save(")
    if save_pos < 0:
        return fail("Could not locate gui_settings_save() in gui_settings.c")
    ensure_pos = source.find("gui_settings_ensure_parent_dirs(path)", save_pos)
    fopen_pos = source.find("FILE *f = fopen(path, \"w\");", save_pos)
    if ensure_pos < 0 or fopen_pos < 0:
        return fail("Missing parent-dir ensure or fopen call in gui_settings_save()")
    if ensure_pos > fopen_pos:
        return fail("gui_settings_save() must ensure parent directories before fopen")
    return 0

def check_flac_large_file_offsets_contract(flac_writer_c_path: Path) -> int:
    source = read_text(flac_writer_c_path)
    required_snippets = [
        "typedef __int64 flac_file_off_t;",
        "#define FLAC_STREAM_FSEEK _fseeki64",
        "#define FLAC_STREAM_FTELL _ftelli64",
        "#define FLAC_STREAM_FSEEK fseeko",
        "#define FLAC_STREAM_FTELL ftello",
        "static FLAC__uint64 flac_stream_max_offset(void)",
    ]
    for snippet in required_snippets:
        if snippet not in source:
            return fail(f"Missing FLAC large-file contract snippet in flac_writer.c: {snippet}")

    seek_start = source.find("static FLAC__StreamEncoderSeekStatus stream_seek_callback(")
    tell_start = source.find("static FLAC__StreamEncoderTellStatus stream_tell_callback(")
    report_start = source.find("static void report_error(")
    if seek_start < 0 or tell_start < 0 or report_start < 0:
        return fail("Missing FLAC stream callback functions in flac_writer.c")

    seek_body = source[seek_start:tell_start]
    tell_body = source[tell_start:report_start]

    if "flac_stream_max_offset()" not in seek_body:
        return fail("stream_seek_callback() must guard against out-of-range large offsets")
    if "FLAC_STREAM_FSEEK(" not in seek_body:
        return fail("stream_seek_callback() must use FLAC_STREAM_FSEEK macro")
    if "fseek(" in seek_body:
        return fail("stream_seek_callback() must not use plain fseek()")

    if "FLAC_STREAM_FTELL(" not in tell_body:
        return fail("stream_tell_callback() must use FLAC_STREAM_FTELL macro")
    if "ftell(" in tell_body:
        return fail("stream_tell_callback() must not use plain ftell()")
    return 0


def check_apprun_static_contract(repo_root: Path) -> int:
    apprun_path = repo_root / APPIMAGE_ASSET_DIR / "AppRun"
    if not apprun_path.exists():
        return fail(f"Missing AppRun: {apprun_path}")
    apprun = read_text(apprun_path)
    # Explanatory comments legitimately mention things the code must not do
    # (e.g. "no set -e"), so scan code lines only.
    code = "\n".join(line for line in apprun.splitlines()
                     if not line.lstrip().startswith("#"))
    required_snippets = [
        "misrc_gui_integrate",
        "--create-shortcut",
        "MISRC_GUI_NO_INTEGRATION",
        "TryExec=${APPIMAGE}",
        "Icon=misrc",
        "StartupNotify=true",
        "update-desktop-database",
        "gtk-update-icon-cache",
    ]
    for snippet in required_snippets:
        if snippet not in code:
            return fail(f"AppRun shortcut contract is missing snippet: {snippet}")
    forbidden_snippets = {
        "set -e": "desktop integration must never be able to block launching the app",
        ".local/bin": "no ~/.local/bin AppImage symlink side effect",
        "%U": "the GUI takes no file arguments; a dropped file would route into CLI capture mode",
        "X-GNOME-WMClass": "legacy key; StartupWMClass is the contract",
        "--version": "launcher identity is version-independent; do not run the binary to build it",
    }
    for snippet, why in forbidden_snippets.items():
        if snippet in code:
            return fail(f"AppRun contains forbidden snippet {snippet!r}: {why}")

    # The Desktop icon is user-requested only. Earlier builds copied one to
    # ~/Desktop on every launch, so it reappeared after being deleted.
    func_start = apprun.find("misrc_gui_create_desktop_icon() {")
    if func_start < 0:
        return fail("AppRun is missing misrc_gui_create_desktop_icon()")
    func_end = apprun.find("\n}\n", func_start)
    func_body = apprun[func_start:func_end]
    outside = apprun[:func_start] + apprun[func_end:]
    stray = [line for line in outside.splitlines()
             if "MISRC GUI.desktop" in line and not line.lstrip().startswith("#")]
    if stray:
        return fail("AppRun writes 'MISRC GUI.desktop' outside misrc_gui_create_desktop_icon()")
    if "MISRC GUI.desktop" not in func_body:
        return fail("misrc_gui_create_desktop_icon() must be the only writer of the Desktop icon")
    calls = [i for i in range(len(apprun)) if apprun.startswith("misrc_gui_create_desktop_icon", i)]
    call_positions = [i for i in calls if i != func_start]
    if len(call_positions) != 1:
        return fail("misrc_gui_create_desktop_icon must be called exactly once (inside --create-shortcut)")
    branch_start = apprun.find('if [ "${1:-}" = "--create-shortcut" ]; then')
    branch_end = apprun.find("\nfi\n", branch_start)
    if branch_start < 0 or not (branch_start < call_positions[0] < branch_end):
        return fail("The Desktop icon may only be created inside the explicit --create-shortcut branch")
    return 0


def _apprun_sandbox(root: Path, apprun_src: Path, icon_path: Path) -> Tuple[Path, Path]:
    """AppDir with stub binaries that log their invocation. Returns (AppRun, calls log)."""
    appdir = root / "AppDir"
    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    calls = root / "calls.log"
    apprun = appdir / "AppRun"
    apprun.write_text(read_text(apprun_src), encoding="utf-8")
    apprun.chmod(apprun.stat().st_mode | stat.S_IXUSR)
    for exe in ("misrc_gui", "misrc_capture", "misrc_extract"):
        exe_path = appdir / "usr/bin" / exe
        exe_path.write_text(
            "#!/usr/bin/env bash\n"
            "printf '%s RESOURCE_NAME=%s args=[%s]\\n' \"$(basename \"$0\")\" "
            "\"${RESOURCE_NAME:-}\" \"$*\" >> \"${MISRC_TEST_CALLS}\"\n"
            "exit 0\n",
            encoding="utf-8",
        )
        exe_path.chmod(exe_path.stat().st_mode | stat.S_IXUSR)
    icon_dir = appdir / "usr/share/icons/hicolor/512x512/apps"
    icon_dir.mkdir(parents=True, exist_ok=True)
    if icon_path.exists():
        shutil.copy2(icon_path, icon_dir / "misrc.png")
    else:
        (icon_dir / "misrc.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    return apprun, calls


def _apprun_home(root: Path, name: str) -> Path:
    """Fresh HOME with a Desktop folder and a deterministic user-dirs config."""
    home = root / name
    (home / "Desktop").mkdir(parents=True, exist_ok=True)
    (home / ".config").mkdir(parents=True, exist_ok=True)
    (home / ".config/user-dirs.dirs").write_text(
        'XDG_DESKTOP_DIR="$HOME/Desktop"\n', encoding="utf-8")
    return home


def _apprun_run(apprun: Path, calls: Path, home: Path, args: List[str],
                appimage: Optional[Path] = None,
                extra_env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
    # Environment is built from scratch so nothing can leak in from the real
    # session (XDG_*, APPIMAGE) and xdg-user-dir can never resolve the real Desktop.
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home),
           "LANG": "C", "MISRC_TEST_CALLS": str(calls)}
    if appimage is not None:
        env["APPIMAGE"] = str(appimage)
    env.update(extra_env or {})
    return subprocess.run(["bash", str(apprun), *args], env=env,
                          capture_output=True, text=True)


def _home_files(home: Path) -> List[str]:
    baseline = {".config/user-dirs.dirs"}
    # By-products of update-desktop-database / gtk-update-icon-cache when those
    # tools exist on the host; benign and not something AppRun writes itself.
    tool_cache_names = {"mimeinfo.cache", "icon-theme.cache"}
    found = []
    for path in sorted(home.rglob("*")):
        if path.is_file() or path.is_symlink():
            rel = path.relative_to(home).as_posix()
            if rel not in baseline and path.name not in tool_cache_names:
                found.append(rel)
    return found


def check_apprun_runtime_behavior(repo_root: Path, icon_path: Path) -> int:
    if not sys.platform.startswith("linux"):
        print("SKIP: AppRun runtime behavior (linux-only)")
        return 0
    if shutil.which("bash") is None:
        print("SKIP: AppRun runtime behavior (bash not available)")
        return 0

    apprun_src = repo_root / APPIMAGE_ASSET_DIR / "AppRun"
    with tempfile.TemporaryDirectory(prefix="misrc_ci_guard_") as temp_root:
        root = Path(temp_root)
        apprun, calls = _apprun_sandbox(root, apprun_src, icon_path)
        try:
            run_checked(["bash", "-n", str(apprun)])
        except subprocess.CalledProcessError as exc:
            return fail(f"AppRun has a shell syntax error: {exc.stderr}")

        # A path with a space and a literal '%' exercises .desktop Exec escaping.
        appimage = root / "MISRC 100% Test Build.AppImage"
        appimage.write_text("fake", encoding="utf-8")
        appimage.chmod(appimage.stat().st_mode | stat.S_IXUSR)

        # 1. Plain GUI launch installs the launcher + icon and nothing else.
        home = _apprun_home(root, "home_plain")
        res = _apprun_run(apprun, calls, home, [], appimage)
        if res.returncode != 0:
            return fail(f"AppRun plain launch failed (rc={res.returncode}): {res.stderr}")
        launcher = home / ".local/share/applications/misrc_gui.desktop"
        icon = home / ".local/share/icons/hicolor/512x512/apps/misrc.png"
        if not launcher.exists() or not icon.exists():
            return fail("AppRun plain launch did not install the launcher and icon")
        text = read_text(launcher)
        for required in (f'Exec="{str(appimage).replace("%", "%%")}"',
                         f"TryExec={appimage}", "Icon=misrc", "Terminal=false",
                         "StartupNotify=true", f"StartupWMClass={APPIMAGE_WM_CLASS}"):
            if required not in text.splitlines():
                return fail(f"Launcher is missing line: {required}\n{text}")
        if "%U" in text or "X-GNOME-WMClass" in text:
            return fail("Launcher must not contain %U or X-GNOME-WMClass")
        if shutil.which("desktop-file-validate"):
            check = subprocess.run(["desktop-file-validate", str(launcher)],
                                   capture_output=True, text=True)
            if check.returncode != 0:
                return fail(f"Generated launcher failed desktop-file-validate: {check.stdout}{check.stderr}")
        unexpected = [f for f in _home_files(home)
                      if f not in (".local/share/applications/misrc_gui.desktop",
                                   ".local/share/icons/hicolor/512x512/apps/misrc.png")]
        if unexpected:
            return fail(f"AppRun plain launch created unexpected files: {unexpected}")
        if any((home / "Desktop").iterdir()):
            return fail("AppRun plain launch must NEVER create a Desktop icon")
        if "misrc_gui RESOURCE_NAME=misrc_gui args=[]" not in read_text(calls):
            return fail("AppRun must exec misrc_gui with RESOURCE_NAME=misrc_gui exported")

        # 2. Idempotent: a second launch must not rewrite anything.
        before = (launcher.stat().st_mtime_ns, icon.stat().st_mtime_ns)
        res = _apprun_run(apprun, calls, home, [], appimage)
        after = (launcher.stat().st_mtime_ns, icon.stat().st_mtime_ns)
        if res.returncode != 0 or before != after:
            return fail("AppRun rewrote an unchanged launcher/icon on a repeat launch")

        # 3. Re-pointing to a different AppImage rewrites Exec (follows the last-run copy).
        other = root / "MISRC Other.AppImage"
        other.write_text("fake", encoding="utf-8")
        _apprun_run(apprun, calls, home, [], other)
        if f'Exec="{other}"' not in read_text(launcher).splitlines():
            return fail("AppRun did not re-point the launcher to the AppImage that ran last")

        # 4. GUI-only flags integrate too (mirrors misrc_gui.c main()).
        for args in (["--debug-view"], ["--config", "/tmp/x.json"], ["--auto-connect", "--config", "/tmp/x.json"]):
            fresh = _apprun_home(root, "home_flags_" + "_".join(a.strip("-/.") for a in args))
            _apprun_run(apprun, calls, fresh, args, appimage)
            if not (fresh / ".local/share/applications/misrc_gui.desktop").exists():
                return fail(f"AppRun did not integrate for GUI launch args: {args}")

        # 5. Non-GUI launches must not touch the desktop at all.
        zero_touch = [
            (["capture", "-h"], appimage, None, "misrc_capture"),
            (["extract", "-h"], appimage, None, "misrc_extract"),
            (["--smoke-test"], appimage, None, "misrc_gui"),
            (["--version"], appimage, None, "misrc_gui"),
            (["--help"], appimage, None, "misrc_gui"),
            (["-a", "-r", "x"], appimage, None, "misrc_gui"),
            ([], None, None, "misrc_gui"),
            ([], appimage, {"MISRC_GUI_NO_INTEGRATION": "1"}, "misrc_gui"),
            ([], root / "bad$name.AppImage", None, "misrc_gui"),
            ([], root / 'bad"name.AppImage', None, "misrc_gui"),
        ]
        for idx, (args, image, extra, expected_exe) in enumerate(zero_touch):
            if image is not None and not image.exists():
                image.write_text("fake", encoding="utf-8")
            fresh = _apprun_home(root, f"home_zero_{idx}")
            calls.write_text("", encoding="utf-8")
            res = _apprun_run(apprun, calls, fresh, args, image, extra)
            if res.returncode != 0:
                return fail(f"AppRun {args} failed (rc={res.returncode}): {res.stderr}")
            touched = _home_files(fresh)
            if touched or any((fresh / "Desktop").iterdir()):
                return fail(f"AppRun {args} (extra={extra}) must not touch the desktop, but created: {touched}")
            if not read_text(calls).startswith(expected_exe + " "):
                return fail(f"AppRun {args} dispatched to the wrong binary; calls: {read_text(calls)!r}")

        # 6. Explicit --create-shortcut is the only way to get a Desktop icon,
        #    and it does not start the GUI.
        home = _apprun_home(root, "home_shortcut")
        calls.write_text("", encoding="utf-8")
        res = _apprun_run(apprun, calls, home, ["--create-shortcut"], appimage)
        desktop_icon = home / "Desktop/MISRC GUI.desktop"
        if res.returncode != 0 or not desktop_icon.exists():
            return fail(f"--create-shortcut did not create the Desktop icon (rc={res.returncode}): {res.stderr}")
        if read_text(desktop_icon) != read_text(home / ".local/share/applications/misrc_gui.desktop"):
            return fail("Desktop icon must be a copy of the installed launcher")
        if not os.access(desktop_icon, os.X_OK):
            return fail("Desktop icon must be executable so the file manager will launch it")
        if read_text(calls).strip():
            return fail("--create-shortcut must not start the GUI")
        res = _apprun_run(apprun, calls, _apprun_home(root, "home_shortcut_noimage"),
                          ["--create-shortcut"], None)
        if res.returncode == 0:
            return fail("--create-shortcut without an AppImage file must fail instead of pretending")

        # 7. Regression: a deleted Desktop icon must STAY deleted (it used to be
        #    copied back on every launch).
        desktop_icon.unlink()
        for _ in range(2):
            res = _apprun_run(apprun, calls, home, [], appimage)
            if res.returncode != 0:
                return fail(f"AppRun launch after deleting the Desktop icon failed: {res.stderr}")
        if desktop_icon.exists() or any((home / "Desktop").iterdir()):
            return fail("REGRESSION: AppRun re-created the deleted Desktop icon on launch")
    return 0


def check_native_wayland_contract(repo_root: Path) -> int:
    """The Linux GUI runs natively on Wayland instead of through Xwayland (on
    2026-10-02 wm's Xwayland died mid-finalize and took the GNOME session with
    it). raylib defaults its bundled GLFW to X11 only, so build-deps-unix.sh
    must turn the Wayland backend on, apply the fork's raylib patch, and fold
    both into the deps stamp -- or a warm host never rebuilds raylib. Every
    window is created after gui_ui_prepare_window_platform() (backend choice,
    app_id, logical-size framebuffer), the app_id matches the launcher's file
    name, and the per-frame scale detection never asks Wayland for a window
    position (a GLFW warning per frame)."""
    deps = read_text(repo_root / "scripts/build-deps-unix.sh")
    for snippet, why in (
        ("-DGLFW_BUILD_WAYLAND=ON", "raylib's GLFW is built without its Wayland backend"),
        ("-DGLFW_BUILD_X11=ON", "raylib's GLFW must keep X11 (cs0's Xvfb, MISRC_GUI_PLATFORM=x11)"),
        ('raylib_platform_args=', "the deps stamp ignores the GLFW backend flags"),
        ('raylib_patch=', "the deps stamp ignores the raylib patch"),
        ('apply --whitespace=nowarn "$RAYLIB_PATCH"', "the raylib patch is never applied"),
        ('checkout --force --detach "$RAYLIB_TAG"', "the raylib clone is not reset before patching"),
    ):
        if snippet not in deps:
            return fail(f"build-deps-unix.sh: {why} (missing {snippet!r})")
    if re.search(r"--parallel\s*$", deps, re.MULTILINE):
        return fail("build-deps-unix.sh: an unnumbered `cmake --build --parallel` is an unbounded make -j")

    patch_path = repo_root / "scripts/patches/raylib-5.5-window-class.patch"
    if not patch_path.exists():
        return fail(f"missing {patch_path.relative_to(repo_root)}")
    patch = read_text(patch_path)
    for snippet in ("void GdhSetWindowPlatformHints(", "GLFW_WAYLAND_APP_ID", "GLFW_SCALE_FRAMEBUFFER"):
        if snippet not in patch:
            return fail(f"raylib patch lost {snippet!r}")
    reset = patch.find("glfwDefaultWindowHints();")
    if reset < 0 or patch.find("glfwWindowHintString(GLFW_WAYLAND_APP_ID", reset) < 0:
        return fail("raylib patch must set the app_id AFTER glfwDefaultWindowHints() resets the hints")
    # A Wayland client-side resize gets no configure event, so GLFW never calls
    # raylib's WindowSizeCallback: without this the startup SetWindowSize left
    # the UI drawn at the old size in the bottom-left corner (2026-10-02).
    if "WindowSizeCallback(platform.handle, width, height)" not in patch:
        return fail("raylib patch: SetWindowSize must run WindowSizeCallback itself on Wayland")

    ui_h = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.h"))
    if '#define GUI_WAYLAND_APP_ID "misrc_gui"' not in ui_h:
        return fail('gui_ui.h: GUI_WAYLAND_APP_ID must be "misrc_gui" (the launcher is misrc_gui.desktop)')
    if "misrc_gui.desktop" not in read_text(repo_root / ".github/workflows/selfhosted-deploy.yml"):
        return fail("selfhosted-deploy.yml no longer installs misrc_gui.desktop, which the app_id names")

    ui_c = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))
    for snippet in ('getenv("MISRC_GUI_PLATFORM")', "__attribute__((weak))", "GdhSetWindowPlatformHints(GUI_WAYLAND_APP_ID, 0)"):
        if snippet not in ui_c:
            return fail(f"gui_ui.c: gui_ui_prepare_window_platform lost {snippet!r}")
    try:
        detect = extract_function_body(ui_c, "int gui_ui_detect_display_scale_percent(void)")
    except RuntimeError as exc:
        return fail(f"gui_ui.c: {exc}")
    call = detect.find("GetCurrentMonitor()")
    gate = detect.find("if (!gui_ui_display_is_wayland())")
    if call >= 0 and not (0 <= gate < call):
        return fail("gui_ui_detect_display_scale_percent calls GetCurrentMonitor() on Wayland "
                    "(a failed window-position query and a GLFW warning every frame)")

    for rel in ("misrc_tools/misrc_gui/core/misrc_gui.c", "misrc_tools/misrc_gui/input/gui_preview_v4l2.c"):
        code = strip_c_comments(read_text(repo_root / rel))
        init = code.find("InitWindow(")
        prep = code.rfind("gui_ui_prepare_window_platform();", 0, init) if init >= 0 else -1
        if init < 0 or prep < 0 or "InitWindow(" in code[prep:init]:
            return fail(f"{rel}: InitWindow() must directly follow gui_ui_prepare_window_platform()")
    return 0


def check_wm_class_consistency(gui_c_path: Path, repo_root: Path) -> int:
    """WM_CLASS is what ties a running window back to its launcher. misrc_gui.c
    creates the window under MISRC_WINDOW_CLASS_TITLE, so every .desktop we write --
    the AppImage's (assets/appimage, shared by the release and the local build),
    the one its AppRun installs, and the self-hosted deploy that installs onto
    workflow-master -- must set StartupWMClass to exactly that string, and must
    never decorate it with a build version, or a launcher written by one build
    stops matching the next build's window and the dock loses the app icon."""
    wm_class = read_gui_window_class_name(gui_c_path)
    deploy_workflow_path = repo_root / ".github/workflows/selfhosted-deploy.yml"
    required_by_surface = {
        repo_root / "assets/appimage/misrc_gui.desktop": (
            f"StartupWMClass={wm_class}",
        ),
        repo_root / "assets/appimage/AppRun": (
            f"StartupWMClass={wm_class}",
        ),
        repo_root / "scripts/build-appimage-local.sh": (
            '"$REPO_ROOT/assets/appimage/misrc_gui.desktop"',
        ),
    }
    for path, required in required_by_surface.items():
        if not path.exists():
            return fail(f"WM_CLASS surface is missing: {path}")
        text = read_text(path)
        for snippet in required:
            if snippet not in text:
                return fail(
                    f"{path.name} does not match MISRC_WINDOW_CLASS_TITLE "
                    f'("{wm_class}") in misrc_gui.c: missing {snippet!r}'
                )
        offenders = find_versioned_wm_class(text)
        if offenders:
            return fail(
                f"{path.name} bakes a build version into WM_CLASS, which breaks the "
                f"launcher-to-window match the dock icon depends on: {offenders}"
            )

    # The deploy installs the launcher this machine actually uses, so assert on the
    # .desktop it generates rather than on the file -- the surrounding comments are
    # free to name the shim they warn against.
    if not deploy_workflow_path.exists():
        return fail(f"WM_CLASS surface is missing: {deploy_workflow_path}")
    entry = extract_deploy_desktop_entry(read_text(deploy_workflow_path))
    for key in ("StartupWMClass", "X-GNOME-WMClass"):
        if f"{key}={wm_class}\n" not in entry + "\n":
            return fail(
                f"selfhosted-deploy.yml installs a .desktop whose {key} does not match "
                f'MISRC_WINDOW_CLASS_TITLE ("{wm_class}") in misrc_gui.c'
            )
    offenders = find_versioned_wm_class(entry)
    if offenders:
        return fail(
            f"selfhosted-deploy.yml bakes a build version into WM_CLASS: {offenders}"
        )
    # RESOURCE_NAME was the pre-constant-class shim. It pins only the instance
    # half of WM_CLASS, and only for a process started through the Exec line that sets
    # it, so the dock icon silently depended on which launcher you clicked. The constant
    # class name replaces it -- reintroducing it re-splits the contract.
    if "RESOURCE_NAME" in entry:
        return fail(
            "selfhosted-deploy.yml reintroduces the RESOURCE_NAME WM_CLASS shim in the "
            f'installed .desktop; StartupWMClass="{wm_class}" already matches however '
            "the app is started"
        )

    # The install must also sweep stale MISRC launchers it did not write. One leftover
    # .desktop with a dead versioned StartupWMClass is enough to put a second, generic
    # "MISRC GUI" in the app grid, which looks exactly like the bug the constant class
    # name fixes. The sweep must never delete the entry the workflow just wrote.
    deploy_text = read_text(deploy_workflow_path)
    required_sweep_snippets = [
        'for stale in "$app_dir"/*.desktop; do',
        'if [ "$stale" = "$app_dir/misrc_gui.desktop" ]; then',
        '"MISRC Capture "*|misrc_gui|misrc-gui)',
        'rm -f "$stale"',
    ]
    for snippet in required_sweep_snippets:
        if snippet not in deploy_text:
            return fail(
                "selfhosted-deploy.yml is missing the stale-launcher sweep, so a dead "
                f"versioned .desktop can shadow the installed one: {snippet}"
            )
    return 0


def check_preview_tap_mux_runtime(repo_root: Path) -> int:
    """The preview publishes its frame tap through a SINGLE atomic slot, so a
    second install silently displaces the first -- starting a stream would stop
    the reference recording receiving frames, with nothing reporting it. The mux
    owns that slot and fans out. Compiles the real module against a harness that
    stands in for the capture thread, so the fan-out, the install/remove
    lifecycle and the removal quiescence guarantee are all exercised."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: preview tap mux runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the preview tap mux runtime guard")
        print("SKIP: preview tap mux runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/preview_tap_mux_harness.c"
    mux_path = repo_root / "misrc_tools/misrc_gui/streaming/gui_preview_tap_mux.c"
    mux_include = repo_root / "misrc_tools/misrc_gui/streaming"

    for required in (harness_path, mux_path):
        if not required.exists():
            return fail(f"Preview tap mux guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_tap_mux_guard_") as temp_root:
        exe_path = Path(temp_root) / "preview_tap_mux_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            "-pthread",
            f"-I{mux_include}",
            str(harness_path),
            str(mux_path),
            "-o",
            str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Preview tap mux harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Preview tap mux harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_cc_record_argv_runtime(repo_root: Path) -> int:
    """The closed-caption ffmpeg command line, asserted token by token, because
    every way of getting it wrong is silent. The VBI node is EXCLUSIVE, so a
    second -i would mean a second open and an EBUSY that costs the capture its
    captions. -raw_timestamps would unrebase the sidecar from t=0 so it no
    longer lines up with the recording -- and it is exactly what someone
    reaches for when trying to 'fix' the constant one-frame offset. A missing
    -y makes ffmpeg block on its own overwrite question with a record session
    open behind it. An input option that drifted after -i silently becomes an
    output option.

    Compiling gui_cc_record.c standalone -- no raylib, no other project source
    -- is itself part of the contract: that module has no project includes, and
    a guard that had to spawn ffmpeg to inspect a command line could not run in
    CI."""
    if not sys.platform.startswith("linux"):
        print("SKIP: closed-caption argv guard (Linux only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the closed-caption argv guard")
        print("SKIP: closed-caption argv guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/gui_cc_record_argv_harness.c"
    module_path = repo_root / "misrc_tools/misrc_gui/output/gui_cc_record.c"
    module_include = repo_root / "misrc_tools/misrc_gui/output"

    for required in (harness_path, module_path):
        if not required.exists():
            return fail(f"Closed-caption argv guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_cc_argv_guard_") as temp_root:
        exe_path = Path(temp_root) / "cc_argv_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{module_include}",
            str(harness_path),
            str(module_path),
            "-o",
            str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(
                "Closed-caption argv harness failed to compile -- if this names a "
                "missing project header, gui_cc_record.c has grown an include it "
                f"must not have:\n{built.stderr.strip()}"
            )
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Closed-caption argv harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_cc_record_ioprio_runtime(repo_root: Path) -> int:
    """The caption recorder's ffmpeg must run one I/O step below the RF writers
    (best-effort level 1), and the thread that spawned it -- the render thread in
    the app -- must keep its own class. Driven with a stand-in ffmpeg script, so
    it needs no capture device and no real ffmpeg."""
    if not sys.platform.startswith("linux"):
        print("SKIP: closed-caption I/O class guard (Linux only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the closed-caption I/O class guard")
        print("SKIP: closed-caption I/O class guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/gui_cc_record_ioprio_harness.c"
    module_path = repo_root / "misrc_tools/misrc_gui/output/gui_cc_record.c"
    module_include = repo_root / "misrc_tools/misrc_gui/output"
    for required in (harness_path, module_path):
        if not required.exists():
            return fail(f"Closed-caption I/O class guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_cc_ioprio_guard_") as temp_root:
        exe_path = Path(temp_root) / "cc_ioprio_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{module_include}",
            str(harness_path),
            str(module_path),
            "-o",
            str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(
                "Closed-caption I/O class harness failed to compile -- if this names a "
                "missing project header, gui_cc_record.c has grown an include it "
                f"must not have:\n{built.stderr.strip()}"
            )
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True, timeout=60)
        if ran.returncode != 0:
            return fail(
                "Closed-caption I/O class harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_cc_record_is_subprocess_only(repo_root: Path) -> int:
    """The caption recorder feeds ffmpeg nothing -- ffmpeg opens the exclusive
    VBI node itself -- so it has no ring, no thread, no preview tap and no
    buffer-manager hook. That is the whole reason captions cannot stall the
    capture thread or the RF writers, and it is a property that would be lost
    quietly: someone adding a drain thread or a tap 'for symmetry' with
    gui_video_record.c would reintroduce exactly the coupling this design
    avoids.

    Also asserts the module includes nothing from the rest of the project. That
    is what lets the argv guard compile it standalone, and it is the invariant
    that rots first -- one convenience include and the guard stops building."""
    rel = "misrc_tools/misrc_gui/output/gui_cc_record.c"
    code = strip_c_comments(read_text(repo_root / rel))
    for banned, why in (
        ("pthread_create", "a thread"),
        ("preview_tap_t", "a preview tap"),
        ("gui_preview_mux_add", "a preview tap"),
        ("gui_preview_hold_acquire", "a preview hold"),
        ("bufmgr_", "a buffer-manager hook"),
        ("BUF_RECORD", "a record ringbuffer"),
        ("socketpair", "a socket transport"),
        ("eventfd", "an eventfd"),
    ):
        if banned in code:
            return fail(
                f"gui_cc_record.c references {banned}: it has grown {why}. The caption "
                "child opens its own device; nothing is fed to it, and nothing in this "
                "module may be able to block the capture path."
            )
    includes = re.findall(r'#include\s+"([^"]+)"', code)
    stray = [i for i in includes if i != "gui_cc_record.h"]
    if stray:
        return fail(
            f"gui_cc_record.c includes {stray} from the project. It must include only its "
            "own header, or the standalone argv guard can no longer compile it."
        )
    return 0


def check_cc_record_probes_never_assumes(repo_root: Path) -> int:
    """v4l2vbi is not a stock ffmpeg input device. A rebuild without it makes
    the device vanish with no other symptom, so its presence is probed and
    never assumed -- and specifically NOT probed with `ffmpeg -h demuxer=...`,
    which exits 0 even for a format that does not exist and would therefore arm
    the toggle on every host where the device is absent.

    The UI render window and the record preflight must both consult the probe:
    the first so the toggle cannot be armed into a state that would refuse a
    recording, the second so a stale OK cannot start one."""
    rel = "misrc_tools/misrc_gui/output/gui_cc_record.c"
    code = strip_c_comments(read_text(repo_root / rel))
    if "-devices" not in code:
        return fail("gui_cc_record.c never interrogates ffmpeg -devices")
    if '" v4l2vbi "' not in code:
        return fail(
            'gui_cc_record.c must match " v4l2vbi " with its surrounding spaces, or a '
            "future v4l2vbi_something would satisfy the probe"
        )
    if "-h demuxer=" in code:
        return fail(
            "gui_cc_record.c probes with `-h demuxer=`, which exits 0 for formats that do "
            "not exist; use -devices"
        )
    if "install.sh" in code:
        return fail(
            "gui_cc_record.c references install.sh, a path that exists on no other "
            "machine; this module is upstream-bound and must probe ffmpeg directly"
        )
    # The captions toggle lives in the USB Reference Video dialog now.
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_usbref_settings.c"))
    if "gui_cc_record_probe()" not in ui:
        return fail("gui_usbref_settings.c never calls gui_cc_record_probe(); "
                    "the toggle cannot grey out")
    rec = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c"))
    if "gui_cc_record_probe()" not in rec:
        return fail("gui_record.c never probes before starting captions")
    return 0


def check_cc_preflight_refuses_before_files(repo_root: Path) -> int:
    """Both optional output streams are preflighted before the session is
    allocated, so a refusal leaves nothing to unwind -- no file open, no writer
    thread, no session log, no child. Moving either check after the session
    exists would mean adding teardown to every failure path below it, and the
    cost of getting that wrong is a half-started recording."""
    code = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c"))
    start = code.find("gui_record_start_confirmed")
    if start < 0:
        return fail("gui_record_start_confirmed not found")
    body = code[start:]
    alloc = body.find("calloc(1, sizeof(*ses))")
    if alloc < 0:
        return fail("the session allocation was not found in gui_record_start_confirmed")
    for field, what in (
        ("settings.usbref_cc_enabled", "closed captions"),
        ("settings.video_record_enabled", "reference video"),
    ):
        at = body.find(field)
        if at < 0:
            return fail(f"gui_record_start_confirmed never preflights {what}")
        if at > alloc:
            return fail(
                f"the {what} preflight runs AFTER the session is allocated, so a refusal "
                "would have to unwind an open file and a live writer thread"
            )
    return 0


def check_cc_sidecar_in_overwrite_set(repo_root: Path) -> int:
    """Every output the recorder will write must be in the overwrite prompt's
    stat() set and named in its message. An output missing from it is an output
    silently clobbered -- which happened to the reference video once already,
    and the prompt still said 'the following files will be overwritten' while
    listing only the RF channels."""
    code = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c"))
    start = code.find("int gui_record_start(gui_app_t *app)")
    if start < 0:
        return fail("gui_record_start not found")
    body = code[start:start + 12000]
    if "settings.usbref_cc_filename" not in body:
        return fail("gui_record_start never builds the caption path for the overwrite check")
    if "file_cc_exists" not in body:
        return fail("gui_record_start never stat()s the caption sidecar")
    if "path_cc, s_finalizing->path_cc" not in body.replace("\n", " ").replace("  ", " "):
        if "s_finalizing->path_cc" not in body:
            return fail(
                "a finalizing session's caption path is not checked, so a new recording "
                "could reuse a file the previous one is still writing"
            )
    if "CAPTIONS:" not in body:
        return fail("the overwrite prompt does not name the caption sidecar it would replace")
    if "VIDEO:" not in body:
        return fail("the overwrite prompt does not name the reference video it would replace")
    return 0


def check_record_start_refuses_while_finalizing(repo_root: Path) -> int:
    """No new recording while the previous one finalizes, as upstream does.
    The fork once allowed a start once the writers drained, which turned the
    orange "Finalize" button into a Record button: on 2026-10-02 a click meant
    to wait for finalize went down that path seconds before wm's Xwayland died
    and took the unfinished capture with it. Every start path ends in
    gui_record_start, so the refusal must sit there, before any output path is
    built, and the button must not read as an action while it holds."""
    code = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c"))
    try:
        body = extract_function_body(code, "int gui_record_start(gui_app_t *app)")
    except RuntimeError as exc:
        return fail(f"gui_record.c: {exc}")
    gate = body.find("if (gui_record_is_finalizing())")
    if gate < 0:
        return fail("gui_record_start does not refuse while gui_record_is_finalizing()")
    for later in ("gui_record_apply_auto_names(app)", "settings.output_filename_a", "gui_record_start_confirmed(app)"):
        at = body.find(later)
        if 0 <= at < gate:
            return fail(f"gui_record_start reaches {later} before the finalize refusal")
    if "->drained" in body:
        return fail("gui_record_start gates on drained again: a start during finalize's metadata tail is back")
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))
    render_pos = ui.find('CLAY(CLAY_ID("RecordButton")')
    window = ui[max(0, render_pos - 2600):render_pos]
    if '"Finalize"' in window or '"Fin"' in window:
        return fail('gui_ui.c: the Record button says "Finalize" again, which reads as an action')
    return 0


def check_cc_settings_have_defaults_and_rows(repo_root: Path) -> int:
    """A settings field with a default and no table row, or a row and no
    default, is silent: it appears to work until the file is reloaded and is
    then quietly the default again. The generic coverage guard catches a
    missing row; this names the caption fields so a half-added setting cannot
    hide behind a passing suite."""
    header = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.h")
    table = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c")
    for field, key in (
        ("usbref_cc_enabled", "usbref_cc_enabled"),
        ("usbref_cc_filename", "usbref_cc_filename"),
        ("usbref_cc_tag", "usbref_cc_tag"),
        ("usbref_cc_vbi_device", "usbref_cc_vbi_device"),
    ):
        if field not in header:
            return fail(f"gui_settings.h has no {field}")
        if f'"{key}"' not in table:
            return fail(f"gui_settings_table.c has no row for {key}")
        if f"settings->{field}" not in table:
            return fail(f"gui_settings_init_defaults never sets {field}")
    # Both auto-namers derive the caption name, and each has a tagged and an
    # untagged branch. Requiring merely "an .scc appears somewhere" would let one
    # branch drift to another extension unnoticed -- so check EVERY occurrence,
    # and that both branches are present in both files. The muxer writes
    # Scenarist SCC; a name with any other extension is a lie about the content.
    rec = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c")
    for rel, text, fn in (
        ("gui_settings_table.c", table, "gui_settings_refresh_auto_names"),
        ("gui_record.c", rec, "gui_record_apply_auto_names"),
    ):
        exts = re.findall(r"_captions\.(\w+)", text)
        if len(exts) < 2:
            return fail(
                f"{rel}: {fn} has {len(exts)} caption-name branch(es), expected 2 "
                "(tagged and untagged)"
            )
        wrong = sorted({e for e in exts if e != "scc"})
        if wrong:
            return fail(
                f"{rel}: {fn} derives a caption filename ending in {wrong}, but the "
                "muxer writes Scenarist SCC"
            )
    return 0


def check_mediamtx_config_runtime(repo_root: Path) -> int:
    """The generated mediamtx.yml carries the design's hard requirement: this
    instance must not disturb capture-node's three on the same host. Two ways to
    break it, both silent -- drift onto one of its ports, or forget to disable a
    listener, since mediamtx defaults rtmp/srt/moq to ON and would quietly claim
    1935/8890/8892. Renders the config for both bind modes and asserts neither."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: mediamtx config runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the mediamtx config runtime guard")
        print("SKIP: mediamtx config runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/mediamtx_config_harness.c"
    module_path = repo_root / "misrc_tools/misrc_gui/streaming/gui_mediamtx.c"
    include_dir = repo_root / "misrc_tools/misrc_gui/streaming"

    for required in (harness_path, module_path):
        if not required.exists():
            return fail(f"mediamtx config guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_mediamtx_guard_") as temp_root:
        exe_path = Path(temp_root) / "mediamtx_config_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            str(module_path),
            "-o",
            str(exe_path),
        ]
        # Same reason as the record-ringbuffer harness: a bare _POSIX_C_SOURCE
        # hides INADDR_LOOPBACK (and the rest of the BSD extensions) on macOS,
        # so the module compiles in the real build -- meson adds this on darwin
        # -- but not in this standalone harness.
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"mediamtx config harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "mediamtx config harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )

        # The harness matches substrings, which cannot tell a valid YAML document
        # from an invalid one. An unquoted "::1" in a flow sequence passed every
        # strstr assertion while making the whole config unparseable -- mediamtx
        # would have refused to start and the stream would simply never come up.
        # So the rendered config is handed to a real parser, and the fields that
        # carry the security contract are read back as data rather than as text.
        try:
            import yaml  # noqa: PLC0415
        except ImportError:
            print("SKIP: mediamtx config YAML validation (PyYAML not installed)")
            return 0

        for mode, args in (("loopback", ["--dump"]), ("lan", ["--dump", "lan"])):
            dumped = subprocess.run([str(exe_path)] + args, capture_output=True, text=True)
            if dumped.returncode != 0:
                return fail(f"mediamtx config harness could not dump the {mode} config")
            try:
                doc = yaml.safe_load(dumped.stdout)
            except yaml.YAMLError as exc:
                return fail(
                    f"the generated mediamtx config is not valid YAML in {mode} mode, "
                    f"so mediamtx would refuse to load it:\n{exc}"
                )
            if not isinstance(doc, dict):
                return fail(f"the generated {mode} config did not parse to a mapping")

            users = doc.get("authInternalUsers")
            if not isinstance(users, list) or not users:
                return fail(f"{mode}: no authInternalUsers block; mediamtx would fall "
                            "back to its default of anyone may publish and read")

            publishers = [u for u in users
                          if any(p.get("action") == "publish"
                                 for p in (u.get("permissions") or []))]
            if len(publishers) != 1:
                return fail(f"{mode}: expected exactly one entry granting publish, "
                            f"found {len(publishers)}")
            ips = publishers[0].get("ips")
            if sorted(ips or []) != ["127.0.0.1", "::1"]:
                return fail(
                    f"{mode}: publish is granted to ips={ips!r}. It must be loopback "
                    "only -- anything wider lets a machine on the network displace "
                    "the publisher and put its own video in front of the operator."
                )

            # The metrics principal, like the publisher, is loopback-only. The
            # endpoint is already bound to 127.0.0.1 so this is defence in depth
            # -- but a widened allow-list is exactly the kind of edit that looks
            # harmless and removes the second layer.
            metric_users = [u for u in users
                            if any(p.get("action") == "metrics"
                                   for p in (u.get("permissions") or []))]
            if len(metric_users) != 1:
                return fail(f"{mode}: expected exactly one entry granting metrics, "
                            f"found {len(metric_users)}")
            mips = metric_users[0].get("ips")
            if sorted(mips or []) != ["127.0.0.1", "::1"]:
                return fail(
                    f"{mode}: metrics is granted to ips={mips!r}; it must be loopback "
                    "only, the same as publishing"
                )

            path_cfg = (doc.get("paths") or {}).get("misrc-preview") or {}
            if path_cfg.get("overridePublisher") is not False:
                return fail(f"{mode}: overridePublisher is "
                            f"{path_cfg.get('overridePublisher')!r}, must be no")
            if path_cfg.get("maxReaders") in (None, 0):
                return fail(f"{mode}: maxReaders is {path_cfg.get('maxReaders')!r}; "
                            "mediamtx reads 0 as unlimited")
            for endpoint in ("api", "pprof", "playback"):
                if doc.get(endpoint) is not False:
                    return fail(f"{mode}: {endpoint} is {doc.get(endpoint)!r}, must be "
                                "explicitly no rather than left to a default")
            # metrics is served on purpose -- it is the only source of the real
            # published bitrate -- but it must never follow the bind mode.
            if doc.get("metrics") is not True:
                return fail(f"{mode}: metrics is {doc.get('metrics')!r}; the panel "
                            "reads the stream bitrate from it")
            addr = doc.get("metricsAddress") or ""
            if not str(addr).startswith("127.0.0.1:"):
                return fail(
                    f"{mode}: metricsAddress is {addr!r}. It must be loopback in BOTH "
                    "modes -- on a LAN interface it would hand anyone who asked the "
                    "session addresses and traffic volumes of a customer's tape."
                )
    return 0


def check_alsa_device_resolution(repo_root: Path) -> int:
    """The stream's audio must come from the SAME physical device as its picture,
    or A/V sync stops being free. On this host the CXADC clock-gen -- the RF audio
    path, on a different clock -- sits one USB port away on the same controller,
    so anything coarser than a full bus-address match streams the wrong audio.
    Card indices are never used: they move across reboots and replugs."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: alsa device resolution guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the alsa device resolution guard")
        print("SKIP: alsa device resolution guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/alsa_device_resolve_harness.c"
    module_path = repo_root / "misrc_tools/misrc_gui/streaming/gui_alsa_device.c"
    include_dir = repo_root / "misrc_tools/misrc_gui/streaming"

    for required in (harness_path, module_path):
        if not required.exists():
            return fail(f"alsa device guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_alsa_guard_") as temp_root:
        exe_path = Path(temp_root) / "alsa_device_guard"
        compile_cmd = [
            cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE",
            f"-I{include_dir}", str(harness_path), str(module_path), "-o", str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"alsa device harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(f"alsa device harness failed:\n{ran.stdout.strip()}\n{ran.stderr.strip()}")
    return 0


def check_alsa_never_stores_a_card_index(repo_root: Path) -> int:
    """A stored hw:N,0 breaks intermittently across reboots and replugs, pointing
    at whatever card inherited the number. Resolution is by USB bus address, and
    the emitted device name must always be plughw:CARD=<name>."""
    module = read_text(repo_root / "misrc_tools/misrc_gui/streaming/gui_alsa_device.c")
    code = strip_c_comments(module)
    if "plughw:CARD=%s,DEV=0" not in code:
        return fail("gui_alsa_device.c no longer emits a name-based plughw:CARD= device")
    for banned in ('"hw:%d', "'hw:%d", '"hw:%u'):
        if banned in code:
            return fail(f"gui_alsa_device.c builds an index-based ALSA device name: {banned}")
    return 0



def check_cxadc_audio_probe_prefers_stable_card_id(repo_root: Path) -> int:
    """The clockgen probe must address a card by its stable ALSA id
    (hw:CARD=wm_cg_audio), not its index. Indices move across reboots and
    replugs, so an index-built device name points at whatever card inherited
    the number. Index forms are tolerated only as a last resort, after the
    id forms have been tried."""
    module = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")
    code = strip_c_comments(module)

    if "snd_ctl_card_info_get_id" not in code:
        return fail("gui_cxadc.c no longer resolves the card's stable ALSA id")

    marker = "static int cxadc_try_open_card_devices"
    start = code.find(marker)
    if start < 0:
        return fail("gui_cxadc.c no longer has cxadc_try_open_card_devices()")
    end = code.find("\n}\n", start)
    body = code[start:end if end > 0 else len(code)]

    id_form = body.find('"%s:CARD=%s"')
    if id_form < 0:
        return fail("cxadc_try_open_card_devices() no longer builds a name-based CARD= device")
    index_form = body.find('"hw:%d"')
    if index_form >= 0 and index_form < id_form:
        return fail("cxadc_try_open_card_devices() tries an index-based device before the stable id")
    return 0

def check_preview_tap_single_slot_contract(repo_root: Path) -> int:
    """Application code must register through the mux, never install the raw tap
    directly -- a direct install displaces whatever the mux published and takes
    every other consumer offline silently. Only the preview module (which owns
    the slot) and the mux (which owns the fan-out) may call it."""
    allowed = {
        repo_root / "misrc_tools/misrc_gui/input/gui_preview_v4l2.c",
        repo_root / "misrc_tools/misrc_gui/input/gui_preview_v4l2.h",
        repo_root / "misrc_tools/misrc_gui/input/gui_preview_tap.h",
        repo_root / "misrc_tools/misrc_gui/streaming/gui_preview_tap_mux.c",
        repo_root / "misrc_tools/misrc_gui/streaming/gui_preview_tap_mux.h",
        repo_root / "misrc_tools/test/preview_tap_mux_harness.c",
    }
    gui_root = repo_root / "misrc_tools/misrc_gui"
    offenders = []
    for path in sorted(gui_root.rglob("*.[ch]")):
        if path in allowed:
            continue
        # Scan code, not prose: a comment explaining why the raw tap is wrong
        # is exactly what a file that correctly uses the mux should contain.
        code = strip_c_comments(read_text(path))
        if "gui_preview_tap_install(" in code or "gui_preview_tap_remove(" in code:
            offenders.append(str(path.relative_to(repo_root)))
    if offenders:
        return fail(
            "These files install the raw preview tap directly, which displaces the "
            f"mux and silently takes other consumers offline; use "
            f"gui_preview_mux_add/remove instead: {offenders}"
        )
    return 0


def check_bundled_mediamtx_contract(repo_root: Path) -> int:
    """A bundled binary is a supply-chain dependency of every release, so the
    version and hashes are pinned in the repo and the download is verified
    against them. Verifying against a checksums file fetched from the same
    release proves only that the transfer worked. The fork's AppImage is the
    one scripts/build-appimage-local.sh builds; upstream's build.yml (never
    edited here) packages its own release AppImage without a server."""
    fetch = repo_root / "scripts/fetch-mediamtx.sh"
    if not fetch.exists():
        return fail("scripts/fetch-mediamtx.sh is missing; the AppImage cannot bundle mediamtx")
    text = read_text(fetch)

    version = re.search(r'^MEDIAMTX_VERSION="(v[0-9]+\.[0-9]+\.[0-9]+)"', text, re.M)
    if not version:
        return fail("fetch-mediamtx.sh must pin an exact MEDIAMTX_VERSION (vX.Y.Z)")

    for arch in ("x86_64", "aarch64"):
        m = re.search(rf'^SHA256_{arch}="([0-9a-f]{{64}})"', text, re.M)
        if not m:
            return fail(f"fetch-mediamtx.sh must pin a 64-hex sha256 for {arch}")

    if "sha256sum --check" not in text:
        return fail("fetch-mediamtx.sh must verify the download against its pinned sha256")
    if "exit 1" not in text:
        return fail("fetch-mediamtx.sh must refuse to stage on a checksum mismatch")
    # The upstream checksums file is not an acceptable substitute for the pin.
    if "checksums.sha256" in strip_shell_comments(text):
        return fail(
            "fetch-mediamtx.sh verifies against the release's own checksums file; "
            "pin the hashes in this repo instead"
        )

    for consumer, label in (
        (repo_root / "scripts/build-appimage-local.sh", "the local AppImage"),
    ):
        if "fetch-mediamtx.sh" not in read_text(consumer):
            return fail(f"{label} does not stage mediamtx via scripts/fetch-mediamtx.sh")
    return 0


# Every key the hand-written settings writer emitted at v1.1.8, before the
# descriptor table replaced it. The table may add keys (an old build ignores
# what it does not know) but must never rename or drop one: a file written by
# the new build has to load in the previous one, or a rollback loses settings.
#
# ONE deliberate exception, taken knowingly: the USB reference video, RTSP and
# caption keys were renamed to a `usbref_` namespace when that UI moved into its
# own dialog. They are fork-only -- upstream has no settings table at all and
# knows none of these keys -- so the compatibility this list protects is the
# fork's own, between fork builds. The cost is real and was accepted: a machine
# on an older build loses these settings, and in net mode a renamed build and an
# un-renamed one silently do not exchange them. Deploy wm and cs0 together.
# The entries below carry the new names, so the contract holds from here on.
SETTINGS_KEYS_V1_1_8 = (
    "device_index", "output_path", "auto_names_enabled", "output_base_name",
    "append_timestamp_on_capture_start", "rf_bits_a", "rf_bits_b", "cxadc_tenbit_mode_a",
    "cxadc_tenbit_mode_b", "rf_tag_a", "rf_tag_b", "output_filename_a", "output_filename_b",
    "capture_a", "capture_b", "sample_count", "capture_time", "overwrite_files", "aux_filename",
    "raw_filename", "audio_4ch_filename", "video_filename", "video_output_tag", "ffmpeg_path",
    "audio_2ch_12_filename", "audio_2ch_34_filename", "audio_1ch_1_filename",
    "audio_1ch_2_filename", "audio_1ch_3_filename", "audio_1ch_4_filename", "audio_1ch_1_label",
    "audio_1ch_2_label", "audio_1ch_3_label", "audio_1ch_4_label", "audio_tag_4ch",
    "audio_tag_2ch_12", "audio_tag_2ch_34", "enable_audio_4ch", "video_record_enabled",
    "usbref_codec", "usbref_device_path", "usbref_rtsp_enabled", "usbref_rtsp_lan",
    "usbref_rtsp_port", "usbref_rtsp_encoder", "usbref_rtsp_bitrate_kbps",
    "usbref_rtsp_deinterlace", "usbref_rtsp_lan_acknowledged", "usbref_rtsp_password", "usbref_mediamtx_path",
    "usbref_rtsp_audio_device", "enable_audio_2ch_12", "enable_audio_2ch_34", "audio_monitor_playback",
    "audio_monitor_ch34", "misrc_mode", "misrc_v15_v25_ab_swap", "stop_on_dropout",
    "level_autostop_enabled", "level_autostop_level_str", "level_autostop_duration_str",
    "ingest_project", "ingest_tape_id", "ingest_tape_format", "ingest_tape_size",
    "ingest_tape_speed", "ingest_tape_condition", "ingest_operator", "ingest_location",
    "ingest_notes", "enable_audio_1ch_1", "enable_audio_1ch_2", "enable_audio_1ch_3",
    "enable_audio_1ch_4", "pad_lower_bits", "show_peak_levels", "suppress_clip_a",
    "suppress_clip_b", "reduce_8bit_a", "reduce_8bit_b", "enable_resample_a", "enable_resample_b",
    "resample_rate_a", "resample_rate_b", "resample_quality_a", "resample_quality_b",
    "resample_gain_a", "resample_gain_b", "ddd_decimation", "use_flac", "flac_12bit", "flac_level",
    "flac_verification", "flac_threads", "flac_affinity_enabled", "flac_affinity_cpu_list",
    "show_grid", "time_scale", "amplitude_scale", "ui_scale_percent", "ui_scale_auto",
    "discover_simple_capture", "show_core_pinning_in_settings", "memory_budget_gb",
    "update_last_check_unix_s", "update_last_release_tag", "update_available_cached",
    "rtlsdr_freq_hz", "rtlsdr_gain_mode", "rtlsdr_gain_tenths_db", "rtlsdr_sample_rate_hz",
    "rtlsdr_agc", "rtlsdr_offset_corr", "demod_mode", "demod_bandwidth_hz", "demod_squelch",
    "demod_volume", "demod_output_pair", "net_mode", "net_server_port", "net_server_port_str",
    "net_client_host", "net_client_port", "net_client_port_str", "playback_file_a",
    "playback_file_b",
)

# Keys deliberately RETIRED by the fork, kept out of the "no v1.1.8 key may be
# dropped" rule. SETTINGS_KEYS_V1_1_8 stays the historical record of what that
# release wrote; a key leaves the written set only by being listed here, with
# the reason. A retired key may never come back as a table row (that would
# re-persist what was retired).
#
# The nine ingest_* keys: capture metadata moved out of gui_settings_t into
# core/gui_capture_meta.h -- per-run, linked from a --session asset or typed
# for one unlinked run, and never saved. An older build loading a newer file
# simply sees them absent (empty strings, their default); a newer build
# loading an older file skips them as unknown keys.
SETTINGS_KEYS_RETIRED = frozenset((
    "ingest_project", "ingest_tape_id", "ingest_tape_format", "ingest_tape_size",
    "ingest_tape_speed", "ingest_tape_condition", "ingest_operator", "ingest_location",
    "ingest_notes",
))

# Fields that describe the machine running this GUI rather than the capture,
# so a net client keeps its own and never sends them to a server.
SETTINGS_CLIENT_LOCAL_KEYS = frozenset((
    "audio_monitor_playback", "audio_monitor_ch34", "show_grid", "time_scale", "amplitude_scale",
    "ui_scale_percent", "ui_scale_auto", "show_core_pinning_in_settings", "memory_budget_gb",
    "update_last_check_unix_s", "update_last_release_tag", "update_available_cached",
    "demod_mode", "demod_bandwidth_hz", "demod_squelch", "demod_volume", "demod_output_pair",
    "net_mode", "net_server_port", "net_server_port_str", "net_client_host", "net_client_port",
    "net_client_port_str", "playback_file_a", "playback_file_b",
    "waveform_scale_mode", "net_client_record_local", "ui_zoom_percent",
))

# Struct fields deliberately not persisted (the duration limits are forced to
# 0 on every load; the feature left the UI).
SETTINGS_UNPERSISTED_FIELDS = frozenset(("capture_limit_seconds", "record_limit_seconds"))


def check_settings_table_covers_struct(repo_root: Path) -> int:
    """gui_settings_t is written, read, published and set through the one
    descriptor table in gui_settings_table.c. A field added to the struct but
    not the table is the old failure in a new place: it looks like a setting,
    it is never saved, and it is quietly the default after every restart. So:
    every struct field has a table row (or is in the explicit unpersisted
    list), every row names a real field, keys are unique, the v1.1.8 key set
    is still written (a rename or a drop breaks rollback), the client-local
    set is exactly the agreed one, and the DdD row stays under ENABLE_DDD."""
    header = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.h")
    table = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c")

    m = re.search(r"typedef struct \{(.*?)\} gui_settings_t;", header, re.S)
    if not m:
        return fail("gui_settings.h no longer defines gui_settings_t")
    body = strip_c_comments(m.group(1))
    members = re.findall(
        r"^\s*[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)*;",
        body, re.M)
    if len(members) < 100:
        return fail(f"gui_settings.h: parsed only {len(members)} gui_settings_t fields; the parser is stale")

    rows = []
    for line in strip_c_comments(table).splitlines():
        r = re.match(r'\s*GS_[A-Z0-9]+\s*\(\s*"([a-z0-9_]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)(.*)\)\s*,?\s*$', line)
        if r:
            rows.append((r.group(1), r.group(2), r.group(3)))
    if len(rows) < 100:
        return fail(f"gui_settings_table.c: parsed only {len(rows)} table rows; the parser is stale")

    keys = [k for k, _, _ in rows]
    dups = sorted({k for k in keys if keys.count(k) > 1})
    if dups:
        return fail(f"gui_settings_table.c has duplicate keys: {', '.join(dups)}")

    struct_fields = set(members)
    row_fields = {f for _, f, _ in rows}
    unknown = sorted(row_fields - struct_fields)
    if unknown:
        return fail(f"gui_settings_table.c names fields gui_settings_t does not have: {', '.join(unknown)}")
    missing = sorted(struct_fields - row_fields - SETTINGS_UNPERSISTED_FIELDS)
    if missing:
        return fail(
            f"gui_settings_t fields with no table row: {', '.join(missing)}. Add a GS_* row to "
            "gui_settings_table.c (or, if the field must never persist, to SETTINGS_UNPERSISTED_FIELDS)."
        )

    revived = sorted(SETTINGS_KEYS_RETIRED & set(keys))
    if revived:
        return fail(
            f"retired settings keys are table rows again: {', '.join(revived)}. They were retired on "
            "purpose (see SETTINGS_KEYS_RETIRED); capture metadata lives in core/gui_capture_meta.h "
            "and is never saved."
        )
    written = {k for k, _, rest in rows if "GS_LOAD_ONLY" not in rest}
    dropped = sorted(set(SETTINGS_KEYS_V1_1_8) - SETTINGS_KEYS_RETIRED - written)
    if dropped:
        return fail(
            f"settings keys written at v1.1.8 are no longer written: {', '.join(dropped)}. "
            "A rename or a drop breaks loading the new file in an older build."
        )

    local = {k for k, _, rest in rows if ("GS_CLIENT_LOCAL" in rest or "GS_LOCAL" in rest)}
    if local != SETTINGS_CLIENT_LOCAL_KEYS:
        extra = sorted(local - SETTINGS_CLIENT_LOCAL_KEYS)
        lost = sorted(SETTINGS_CLIENT_LOCAL_KEYS - local)
        return fail(
            "the client-local key set drifted from the agreed one: "
            f"unexpectedly local: {extra or 'none'}; no longer local: {lost or 'none'}"
        )

    ddd_blocks = re.findall(r"#ifdef ENABLE_DDD(.*?)#endif", table, re.S)
    if not any('"ddd_decimation"' in block for block in ddd_blocks):
        return fail("gui_settings_table.c: the ddd_decimation row must sit inside #ifdef ENABLE_DDD")
    return 0


def check_settings_roundtrip_runtime(repo_root: Path) -> int:
    """Compiles gui_settings_table.c standalone (no raylib) and drives it with
    a settings file in the exact format the hand-written writer produced at
    v1.1.8: load -> save -> load must be a fixed point, every key written once
    (the old writer emitted nine twice), unknown keys dropped, the load-time
    clamps and migrations intact, the JSON snapshot lossless, and the strict
    (network) parser refusing what the raw file dialect cannot hold. This is
    what lets the table replace the writer without an existing settings file
    changing meaning."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: settings round-trip runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the settings round-trip runtime guard")
        print("SKIP: settings round-trip runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/gui_settings_roundtrip_harness.c"
    table_path = repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c"
    scale_path = repo_root / "misrc_tools/misrc_gui/ui/gui_ui_scale.c"
    fixture_path = repo_root / "misrc_tools/test/fixtures/settings_v1_1_8.json"
    for required in (harness_path, table_path, scale_path, fixture_path):
        if not required.exists():
            return fail(f"Settings round-trip guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_settings_rt_guard_") as temp_root:
        exe_path = Path(temp_root) / "settings_roundtrip_guard"
        # No -Werror: the auto-name formatter's snprintf calls are bounded by
        # MAX_FILENAME_LEN on purpose and trip -Wformat-truncation.
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{table_path.parent}",
            f"-I{scale_path.parent}",
            str(harness_path),
            str(table_path),
            str(scale_path),
            "-lm",
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Settings round-trip harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path), str(fixture_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Settings round-trip harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_rtsp_settings_roundtrip(repo_root: Path) -> int:
    """The settings table (gui_settings_table.c) is what the file writer, the
    file reader and the network snapshot all walk, but a field still needs a
    default and a table row, written in two places. A field with one and not
    the other is silent: the setting appears to work until it is reloaded,
    and then it is quietly the default again. Assert every stream setting has
    both."""
    struct_src = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.h")
    table_src = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c")

    fields = sorted(set(
        re.findall(r"\b(usbref_rtsp_[a-z0-9_]+|usbref_mediamtx_path|usbref_device_path)\b(?:\[\d+\])?\s*;",
                   struct_src)
    ))
    if not fields:
        return fail("gui_settings.h declares no rtsp_* stream settings")

    for field in fields:
        defaulted = re.search(rf"settings->{field}(\[0\])?\s*=", table_src) is not None
        tabled = re.search(rf'GS_[A-Z0-9]+\s*\(\s*"{field}"\s*,\s*{field}\b', table_src) is not None

        missing = [name for name, ok in
                   (("a default", defaulted), ("a table row", tabled)) if not ok]
        if missing:
            return fail(
                f"gui_settings_table.c is missing {' and '.join(missing)} for {field}. "
                "A setting present in only one of the two sites is silent: it "
                "appears to work until it is reloaded as the default."
            )
    return 0


def check_streaming_children_yield_to_rf(repo_root: Path) -> int:
    """RTSP fan-out must yield to RF ingest. This app IS the RF recorder, so an
    encoder and a media server competing for the same cores is not a fair fight
    to leave to chance -- the design calls for the nice(+5) posture capture-node
    uses on its ffmpeg children, and it was missing until the concurrency proof
    went looking for it."""
    for rel, define in (
        ("misrc_tools/misrc_gui/streaming/gui_rtsp_stream.c", "RS_CHILD_NICE"),
        ("misrc_tools/misrc_gui/streaming/gui_mediamtx.c", "MTX_CHILD_NICE"),
        ("misrc_tools/misrc_gui/output/gui_cc_record.c", "CC_CHILD_NICE"),
    ):
        code = strip_c_comments(read_text(repo_root / rel))
        m = re.search(rf"#define\s+{define}\s+(\d+)", code)
        if not m:
            return fail(f"{Path(rel).name} does not define {define}")
        if int(m.group(1)) < 1:
            return fail(
                f"{Path(rel).name} sets {define}={m.group(1)}, so its child competes "
                "with RF ingest on equal terms"
            )
        if f"setpriority(PRIO_PROCESS" not in code or define not in code.split("#define")[-1] + code:
            return fail(f"{Path(rel).name} never applies {define} via setpriority()")
        if "setpriority(PRIO_PROCESS, (id_t)pid" not in code:
            return fail(
                f"{Path(rel).name} must nice the SPAWNED CHILD by pid; a 0 pid would "
                "nice this process instead and slow the RF path it is protecting"
            )
    return 0


def check_gdh_host_installer(repo_root: Path) -> int:
    """scripts/gdh-host/install.sh is what the GDH hosts run as root (from a
    root-owned copy) to grant the scheduling limits the capture path needs. Run
    it in --print mode -- no root, nothing written -- and check what it would
    install: the user-manager drop-in lets the GUI's threads reach SCHED_FIFO 99
    and nice -20; the runner drop-in gives CI exactly what the priority
    harnesses lower to (RTPRIO 5, NICE 25) and no more, so a PR's code cannot
    take FIFO 99 on a runner host; cs0's misrc-server unit runs --net-serve as
    the invoking user with the GUI's limits, top CPU and I/O weight, and room
    for a FLAC finalize on stop. It must refuse to install without root and
    never restart a service: user@ and the runner would take the deploy down
    with them."""
    if not sys.platform.startswith("linux"):
        print("SKIP: GDH host installer guard (Linux only)")
        return 0
    script = repo_root / "scripts/gdh-host/install.sh"
    if not script.exists():
        return fail(f"GDH host installer is missing: {script}")

    def render(host: str):
        ran = subprocess.run(["bash", str(script), "--print", host],
                             capture_output=True, text=True)
        if ran.returncode != 0:
            return None, f"install.sh --print {host} exited {ran.returncode}: {ran.stderr.strip()}"
        sections: Dict[str, List[str]] = {}
        current = None
        for line in ran.stdout.splitlines():
            if line.startswith("== "):
                current = line[3:].strip()
                sections[current] = []
            elif current is not None:
                sections[current].append(line.strip())
        return sections, None

    def settings(lines: List[str]) -> Dict[str, str]:
        out: Dict[str, str] = {}
        for line in lines:
            if "=" in line and not line.startswith("#"):
                key, _, value = line.partition("=")
                out[key.strip()] = value.strip()
        return out

    cs0, err = render("cs0")
    if err:
        return fail(err)
    wm, err = render("wm")
    if err:
        return fail(err)

    user_dropin = "/etc/systemd/system/user@.service.d/misrc-limits.conf"
    unit = "/etc/systemd/system/misrc-server.service"
    for host, sections in (("cs0", cs0), ("wm", wm)):
        got = settings(sections.get(user_dropin, []))
        if got.get("LimitRTPRIO") != "99" or got.get("LimitNICE") != "-20":
            return fail(f"install.sh {host}: {user_dropin} must set LimitRTPRIO=99 and "
                        f"LimitNICE=-20, got {got}")
        runner = [k for k in sections if "actions.runner." in k
                  and k.endswith("/misrc-guard-limits.conf")]
        if not runner:
            return fail(f"install.sh {host} installs no runner drop-in (misrc-guard-limits.conf)")
        for key in runner:
            got = settings(sections[key])
            # Exactly what priority_clamp_harness.c / flac_worker_priority_harness.c
            # lower their soft limits to; anything higher hands PR code more.
            if got.get("LimitRTPRIO") != "5" or got.get("LimitNICE") != "25":
                return fail(f"install.sh {host}: {key} must set LimitRTPRIO=5 and "
                            f"LimitNICE=25, got {got}")
            # 2026-09-15: a global OOM killed one small process inside the wm runner's
            # unit; systemd's default OOMPolicy=stop then took the whole runner down
            # and Restart=no left it there, so every fork PR's CI queued. Measured:
            # OOMPolicy=continue keeps the unit running through a child's kill, and
            # Restart=on-failure brings it back when the listener itself dies.
            if got.get("OOMPolicy") != "continue":
                return fail(f"install.sh {host}: {key} must set OOMPolicy=continue so a "
                            f"child's OOM kill does not stop the runner, got {got.get('OOMPolicy')}")
            if got.get("Restart") != "on-failure" or not got.get("RestartSec", "").rstrip("s").isdigit():
                return fail(f"install.sh {host}: {key} must set Restart=on-failure with a "
                            f"RestartSec, got Restart={got.get('Restart')} "
                            f"RestartSec={got.get('RestartSec')}")
    if unit in wm:
        return fail("install.sh wm installs misrc-server.service; the unit is cs0's only")
    if unit not in cs0:
        return fail("install.sh cs0 does not install misrc-server.service")
    body = cs0[unit]
    got = settings(body)
    who = os.environ.get("SUDO_USER") or getpass.getuser()
    exec_start = got.get("ExecStart", "")
    problems = []
    if "--net-serve" not in exec_start or "--config" not in exec_start:
        problems.append(f"ExecStart must run --config ... --net-serve, got '{exec_start}'")
    if any("%h" in line for line in body if not line.startswith("#")):
        problems.append("uses %h, which is root's home in a system unit even with User=")
    if got.get("User") != who:
        problems.append(f"User={got.get('User')}, want the invoking user {who}")
    for key, want in (("LimitRTPRIO", "99"), ("LimitNICE", "-20"), ("CPUWeight", "10000"),
                      ("IOWeight", "10000"), ("Restart", "on-failure"),
                      ("WantedBy", "multi-user.target")):
        if got.get(key) != want:
            problems.append(f"{key}={got.get(key)}, want {want}")
    stop = got.get("TimeoutStopSec", "")
    if not stop.isdigit() or int(stop) < 600:
        problems.append(f"TimeoutStopSec={stop or 'unset'}; a legacy FLAC rewrite on stop "
                        "takes minutes, give it at least 600")
    if problems:
        return fail("install.sh cs0: misrc-server.service " + "; ".join(problems))

    bad = subprocess.run(["bash", str(script), "--print", "nosuchhost"],
                         capture_output=True, text=True)
    if bad.returncode == 0:
        return fail("install.sh accepts an unknown host; it must take only wm or cs0")
    if os.geteuid() != 0:
        real = subprocess.run(["bash", str(script), "wm"], capture_output=True, text=True)
        if real.returncode == 0:
            return fail("install.sh installed without root; it must refuse")
    source = strip_shell_comments(read_text(script))
    if re.search(r"systemctl\s+(restart|try-restart|reload-or-restart|try-reload-or-restart)\b",
                 source):
        return fail("install.sh restarts a service; it may only enable, start and daemon-reload")
    return 0


def check_capture_children_io_class(repo_root: Path) -> int:
    """Every child process the GUI spawns carries an I/O class chosen for it. It
    is set on the spawning thread right before posix_spawn -- the child inherits
    it at fork, so the child's own threads cannot race it -- and taken back
    right after, because that thread is the render thread. The recorders'
    children (the ffmpeg reference video, the caption recorder) sit one step
    below the RF writers, best-effort level 1; the stream's ffmpeg and mediamtx
    take only idle disk time. The caption recorder and mediamtx also have
    runtime checks; the other two need a V4L2 device to start."""
    for rel, define, want in (
        ("misrc_tools/misrc_gui/output/gui_video_record.c", "VR_CHILD_IOPRIO", (2 << 13) | 1),
        ("misrc_tools/misrc_gui/output/gui_cc_record.c", "CC_CHILD_IOPRIO", (2 << 13) | 1),
        ("misrc_tools/misrc_gui/streaming/gui_rtsp_stream.c", "RS_CHILD_IOPRIO", 3 << 13),
        ("misrc_tools/misrc_gui/streaming/gui_mediamtx.c", "MTX_CHILD_IOPRIO", 3 << 13),
    ):
        code = strip_c_comments(read_text(repo_root / rel))
        name = Path(rel).name
        m = re.search(rf"#define\s+{define}\s+\(([\d\s<|()]+)\)", code)
        if not m:
            return fail(f"{name} does not define {define}")
        # "(class << 13) | level" or "class << 13", the kernel's ioprio encoding.
        parts = re.fullmatch(r"(\d+)<<13(?:\|(\d+))?", re.sub(r"[\s()]", "", m.group(1)))
        if not parts:
            return fail(f"{name} writes {define} as '{m.group(1)}', not (class << 13) | level")
        value = (int(parts.group(1)) << 13) | int(parts.group(2) or 0)
        if value != want:
            return fail(f"{name} sets {define} to {value}, want {want}")
        spawns = [mm.start() for mm in re.finditer(r"\bposix_spawnp?\(", code)]
        if not spawns:
            return fail(f"{name} spawns no child any more; update this guard")
        for at in spawns:
            before, after = code[max(0, at - 400):at], code[at:at + 400]
            if not re.search(rf"SYS_ioprio_set[^;]*{define}", before):
                return fail(
                    f"{name} spawns a child without setting {define} on the spawning "
                    "thread first; the child would inherit the render thread's class"
                )
            if "SYS_ioprio_set" not in after:
                return fail(
                    f"{name} never gives the spawning thread its own I/O class back "
                    "after posix_spawn; the render thread would keep the child's"
                )
    return 0


def check_clay_text_outlives_layout(repo_root: Path) -> int:
    """Clay stores the pointer it is handed and reads it back in Clay_EndLayout(),
    which misrc_gui.c calls after the whole layout function has returned. Anything
    with automatic storage duration is freed by then, so the draw pass reads a dead
    stack frame and raylib substitutes '?' for every byte that is not a glyph. The
    failure is quiet and looks like a font problem, which is why it needs a guard:
    the layout pass measures the real string, so the box is the right size and only
    the text inside it is wrong. gui_ui.c states the rule in a comment; this makes
    it enforceable."""
    # Both Clay-drawing UI files: the USB Reference Video dialog hands Clay the
    # same kind of formatted labels, and a dangling one there fails identically.
    paths = [repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c",
             repo_root / "misrc_tools/misrc_gui/ui/gui_usbref_settings.c",
             repo_root / "misrc_tools/misrc_gui/ui/gui_capture_meta_panel.c"]
    failures_before = 0
    for path in paths:
        rc = _check_clay_text_in_file(path)
        if rc != 0:
            failures_before += rc
    return 1 if failures_before else 0


def _check_clay_text_in_file(path: Path) -> int:
    code = strip_c_comments(read_text(path))

    # Declarations inside a function body, i.e. automatic storage. File-scope
    # declarations are unindented and live for the life of the process.
    stack_arrays: dict = {}
    stack_structs: dict = {}
    for lineno, raw in enumerate(code.splitlines(), 1):
        if not raw[:1].isspace():
            continue
        stmt = raw.strip()
        if stmt.startswith("static ") or stmt.startswith("extern "):
            continue
        m = re.match(r"(?:const\s+)?char\s+([A-Za-z_]\w*)\s*\[", stmt)
        if m:
            stack_arrays.setdefault(m.group(1), lineno)
            continue
        # A struct copied onto the stack, e.g. `foo_status_t st = foo_get_status();`
        m = re.match(r"([A-Za-z_]\w*_t)\s+([A-Za-z_]\w*)\s*=", stmt)
        if m:
            stack_structs.setdefault(m.group(2), lineno)

    offenders = []
    for m in re.finditer(r"make_string\(\s*([^();]*?)\s*\)", code):
        arg = m.group(1)
        base = re.match(r"([A-Za-z_]\w*)", arg)
        if not base:
            continue
        name = base.group(1)
        where = code[: m.start()].count("\n") + 1
        if name in stack_arrays:
            offenders.append((where, arg, stack_arrays[name], "stack buffer"))
        elif name in stack_structs and arg != name:
            offenders.append((where, arg, stack_structs[name], "field of a stack struct"))

    if offenders:
        lines = [
            f"  gui_ui.c:{where}: make_string({arg}) reads a {kind} declared at line {decl}"
            for where, arg, decl, kind in sorted(offenders)
        ]
        return fail(
            "CLAY_TEXT was handed storage that does not outlive the layout pass:\n"
            + "\n".join(lines)
            + "\nClay keeps the pointer and dereferences it after Clay_EndLayout() in "
            "misrc_gui.c, by which time the frame is gone. Copy the text into a "
            "`static char` buffer first, as every other dynamic string in this file does."
        )
    return 0


def check_lan_requires_acknowledgement(repo_root: Path) -> int:
    """Switching the stream to LAN puts the tape being captured in front of
    everyone on the network. That is a different kind of act from the toggles
    beside it, so the first one asks. The failure this guards against is someone
    later simplifying the handler back to an unconditional flip -- which reads
    like a tidy-up and silently removes the only thing standing between a click
    and a customer's tape on the network.

    Also asserts the shape of the answer: accepting records consent, declining
    does not. Remembering a "no" would mean the warning never returns."""
    # Moved to the USB Reference Video dialog with the rest of the stream UI.
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_usbref_settings.c"))

    m = re.search(r'Clay_PointerOver\(CLAY_ID\("RtspBindBox"\)\)', ui)
    if not m:
        return fail("the RtspBindBox handler is gone; this guard must move with it")
    handler = ui[m.start():m.start() + 700]

    if "usbref_rtsp_lan_acknowledged" not in handler:
        return fail(
            "the LAN toggle flips without consulting usbref_rtsp_lan_acknowledged, so the "
            "first switch to LAN no longer asks before putting a tape on the network"
        )
    if "s_rtsp_lan_confirm_open" not in handler:
        return fail("the LAN toggle never opens the confirmation")

    # Accepting must set both the consent and the mode.
    accept = re.search(
        r'Clay_PointerOver\(CLAY_ID\("RtspLanConfirmAccept"\)\)\s*\)\s*\{(.*?)\}\s*else',
        ui, re.S)
    if not accept:
        return fail("the confirmation has no Accept branch")
    body = accept.group(1)
    for needed in ("usbref_rtsp_lan_acknowledged = true", "usbref_rtsp_lan = true",
                   "gui_settings_save"):
        if needed not in body:
            return fail(f"the Accept branch does not do `{needed}`")

    # Declining must not be remembered as an answer.
    cancel = re.search(
        r'Clay_PointerOver\(CLAY_ID\("RtspLanConfirmCancel"\)\)\s*\)\s*\{(.*?)\n\s*\}',
        ui, re.S)
    if not cancel:
        return fail("the confirmation has no Cancel branch")
    if "usbref_rtsp_lan_acknowledged" in cancel.group(1):
        return fail(
            "declining the LAN warning writes usbref_rtsp_lan_acknowledged. A refusal is not "
            "consent, and recording it means the warning never comes back."
        )
    if "usbref_rtsp_lan = true" in cancel.group(1):
        return fail("declining the LAN warning still switches to LAN")

    return 0


def check_streaming_writes_are_private(repo_root: Path) -> int:
    """The mediamtx config and ffmpeg's stderr log are written into
    $XDG_RUNTIME_DIR -- or, for a session that has none, into world-writable
    /tmp. Both describe the stream, the config is where a password will live,
    and both were being created 0644 through a path that would follow a symlink.
    Another user could pre-create the directory, read what we wrote, or plant a
    link at our filename and have us truncate a file of their choosing.

    Asserts the two creation sites stay private and refuse to follow links, and
    that the directory is checked rather than assumed to be ours."""
    checks = (
        (
            "misrc_tools/misrc_gui/streaming/gui_mediamtx.c",
            "the generated mediamtx config",
            "mtx.config_path",
        ),
        (
            "misrc_tools/misrc_gui/streaming/gui_rtsp_stream.c",
            "ffmpeg's stderr log",
            "rs.ffmpeg_log",
        ),
    )

    for rel, what, target in checks:
        path = repo_root / rel
        code = strip_c_comments(read_text(path))

        # Find the open() that creates it, and read the flags and mode it passes.
        m = re.search(
            rf"open\w*\([^;]*?{re.escape(target)}\s*,\s*([^;]*?)\)\s*;",
            code,
            re.S,
        )
        if not m:
            return fail(
                f"{Path(rel).name}: could not find the open() that creates {what}; "
                "if it moved, this guard must move with it"
            )
        args = re.sub(r"\s+", " ", m.group(1))

        if "O_NOFOLLOW" not in args:
            return fail(
                f"{Path(rel).name}: {what} is created without O_NOFOLLOW ({args}). "
                "Under /tmp a symlink planted at that filename would redirect the "
                "write to a file the attacker chose and we can reach."
            )
        if "S_IRUSR | S_IWUSR" not in args and "S_IWUSR | S_IRUSR" not in args:
            return fail(
                f"{Path(rel).name}: {what} is not created 0600 ({args}). It describes "
                "the stream, and the config is where the stream password lives."
            )
        for octal in ("0644", "0666", "0640", "0604"):
            if octal in args:
                return fail(f"{Path(rel).name}: {what} is created {octal}")

    # The directory itself has to be verified, not merely mkdir'd.
    mtx = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/streaming/gui_mediamtx.c"))
    if "mtx_dir_is_private" not in mtx:
        return fail(
            "gui_mediamtx.c no longer checks that its runtime directory is private. "
            "mkdir() tolerating EEXIST is not the same as owning what already exists."
        )
    body = re.search(r"static bool mtx_dir_is_private\(const char \*path\)\s*\{(.*?)\n\}",
                     mtx, re.S)
    if not body:
        return fail("mtx_dir_is_private() is missing or its shape changed")
    for needle, why in (
        ("lstat", "must lstat, not stat -- a symlink is not a directory we own"),
        ("S_ISDIR", "must confirm it is a directory"),
        ("geteuid", "must confirm we own it"),
        ("S_IRWXG", "must reject or repair group access"),
        ("S_IRWXO", "must reject or repair world access"),
    ):
        if needle not in body.group(1):
            return fail(f"mtx_dir_is_private() {why}")

    # And it must actually be called by the path that builds the directory.
    dir_fn = re.search(r"static bool mtx_config_dir\(char \*out, size_t cap\)\s*\{(.*?)\n\}",
                       mtx, re.S)
    if not dir_fn:
        return fail("mtx_config_dir() is missing or its shape changed")
    # The result must GATE the return, not merely be mentioned. Naming the
    # function is not calling it: a `(void)mtx_dir_is_private;` satisfies a
    # substring search, keeps -Werror quiet about an unused static, and checks
    # nothing at all. Mutation testing caught exactly that.
    if not re.search(r"return\s+mtx_dir_is_private\s*\(\s*out\s*\)\s*;", dir_fn.group(1)):
        return fail(
            "mtx_config_dir() does not return mtx_dir_is_private(out); the directory "
            "check either is not called or does not decide the outcome"
        )
    return 0


def check_net_controls_publish_effective_mode(repo_root: Path) -> int:
    """A net-mode client runs its own extraction on the raw /rf stream, so its
    A/B swap decision must equal the server's. The server's decision is the
    effective mode (capture_mode_runtime_misrc while recording, else
    user_capture_mode_misrc), which the UI sync and the capture-settings clamp
    force off for CXADC devices while deliberately leaving settings.misrc_mode
    alone. /controls used to publish the setting, so a client of a CXADC server
    swapped channels the server did not. The V1.5/V2.5 wiring flag inverts the
    swap wherever it is applied, so it has to travel with the mode and the
    client has to mirror it."""
    net_c = repo_root / "misrc_tools/misrc_gui/net/gui_net.c"
    if not net_c.exists():
        return fail(f"missing {net_c}")
    code = strip_c_comments(read_text(net_c))

    m = re.search(r"static void server_build_controls\s*\([^)]*\)\s*\{", code)
    if not m:
        return fail("gui_net.c: server_build_controls() not found")
    end = code.find("\n}\n", m.end())
    body = code[m.end():end if end >= 0 else len(code)]
    if "settings.misrc_mode" in body:
        return fail(
            "gui_net.c: server_build_controls() publishes settings.misrc_mode. Publish the "
            "effective mode (capture_mode_runtime_misrc while recording, else "
            "user_capture_mode_misrc); a CXADC server runs with the setting left on."
        )
    for needle in (
        "capture_mode_runtime_misrc",
        "user_capture_mode_misrc",
        '\\"misrc_v15_v25_ab_swap\\"',
        "settings.misrc_v15_v25_ab_swap",
    ):
        if needle not in body:
            return fail(f"gui_net.c: server_build_controls() no longer references {needle}")

    # /settings publishes the same effective mode (server_publish computes it
    # for the published copy the handlers read).
    m_pub = re.search(r"static void server_publish\s*\([^)]*\)\s*\{", code)
    if not m_pub:
        return fail("gui_net.c: server_publish() not found")
    pub_end = code.find("\n}\n", m_pub.end())
    pub_body = code[m_pub.end():pub_end if pub_end >= 0 else len(code)]
    if "capture_mode_runtime_misrc" not in pub_body or "user_capture_mode_misrc" not in pub_body:
        return fail("gui_net.c: server_publish() no longer derives the effective mode for /settings")

    # The client: /settings carries misrc_mode_effective; an older server
    # without /settings still gets mode + swap from /controls. Both land in
    # the STAGED snapshot (never app->settings from the worker), and the
    # main thread feeds the pipeline from it in gui_net_poll_mirror.
    m2 = re.search(r'client_get\s*\([^;]*"/controls"', code)
    if not m2:
        return fail("gui_net.c: the client /controls fallback was not found")
    window = code[m2.end():m2.end() + 1200]
    if not re.search(r'staged_effective_misrc\s*=\s*json_bool\s*\(\s*\w+\s*,\s*"misrc_mode"', window):
        return fail("gui_net.c: the /controls fallback does not stage misrc_mode as the effective mode")
    if not re.search(
        r'staged_settings\.misrc_v15_v25_ab_swap\s*=\s*json_bool\s*\(\s*\w+\s*,\s*"misrc_v15_v25_ab_swap"',
        window,
    ):
        return fail("gui_net.c: the /controls fallback does not stage misrc_v15_v25_ab_swap")
    m3 = re.search(r'json_bool\s*\(\s*\w+\s*,\s*"misrc_mode_effective"', code)
    if not m3:
        return fail("gui_net.c: the client does not read misrc_mode_effective from /settings")
    m4 = re.search(r"void gui_net_poll_mirror\s*\([^)]*\)\s*\{", code)
    if not m4:
        return fail("gui_net.c: gui_net_poll_mirror() not found")
    mirror_body = code[m4.end():code.find("\n}\n", m4.end())]
    if "user_capture_mode_misrc = s_peer_effective_misrc" not in mirror_body or \
       "capture_ab_swap_invert" not in mirror_body:
        return fail("gui_net.c: gui_net_poll_mirror() does not feed the effective mode and the swap flag to the pipeline")

    # The UI's client branch follows that, not a settings field.
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))
    m5 = re.search(r"void gui_ui_sync_capture_mode_state\s*\([^)]*\)\s*\{", ui)
    if not m5:
        return fail("gui_ui.c: gui_ui_sync_capture_mode_state() not found")
    client_branch = ui[m5.end():ui.find("#ifdef ENABLE_DDD", m5.end())]
    if "gui_net_is_client(app)" not in client_branch or "settings.misrc_mode" in client_branch.split("gui_net_is_client(app)")[1]:
        return fail("gui_ui.c: the client branch of gui_ui_sync_capture_mode_state() reads settings.misrc_mode; "
                    "it must follow user_capture_mode_misrc, which the mirror sets from the server's effective mode")
    return 0


def check_net_settings_protocol(repo_root: Path) -> int:
    """The settings setter is applied on the server's MAIN thread only (the
    HTTP handler validates, queues and waits), the HTTP threads read
    published copies, the client worker never writes app->settings, a client
    never persists a server value, /stats carries the recording relay, the
    extraction reads the main-thread swap flag, the two headless modes
    exist and return before InitWindow, and the overwrite prompt travels
    both ways (/stats publishes its text, the client mirrors the dialog and
    answers with /record?confirm=N, resolved on the server's main thread)."""
    net_c = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/net/gui_net.c"))

    def body_of(name: str, src: str, pattern: str = None) -> str:
        m = re.search(pattern or (r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{"), src)
        if not m:
            return ""
        end = src.find("\n}\n", m.end())
        return src[m.end():end if end >= 0 else len(src)]

    dispatch = body_of("server_handle_request", net_c)
    for uri in ("/settings", "/set"):
        if f'strcmp(uri, "{uri}") == 0' not in dispatch:
            return fail(f"gui_net.c: server_handle_request() does not dispatch {uri}")
    rec = dispatch.find('strcmp(uri, "/record") == 0')
    on = dispatch.find('server_parse_arg(query, "on"', rec)
    conf = dispatch.find('server_parse_arg(query, "confirm"', rec)
    if rec < 0 or conf < 0 or on < 0 or conf > on:
        return fail("gui_net.c: /record must parse confirm= before on= (on= defaults to 1 when "
                    "absent, so a confirm query parsed second would start a recording)")
    if "net_cmd_record_confirm" not in dispatch[rec:on]:
        return fail("gui_net.c: /record?confirm= must only raise net_cmd_record_confirm for the main thread")

    setter = body_of("server_handle_set", net_c)
    if not setter:
        return fail("gui_net.c: server_handle_set() not found")
    for status in ("400", "422", "503"):
        if f"server_send_error(fd, {status}" not in setter:
            return fail(f"gui_net.c: server_handle_set() never answers {status}")
    for forbidden in ("gui_settings_save(", "gui_ui_apply_remote_setting(", "gui_settings_apply_key(&app->settings"):
        if forbidden in setter:
            return fail(f"gui_net.c: server_handle_set() runs on an HTTP thread and must not call {forbidden}; "
                        "it validates against the published copy and queues for the main thread")
    if re.search(r"app->settings\.\w+(\[[^\]]*\])*\s*=[^=]", setter):
        return fail("gui_net.c: server_handle_set() assigns app->settings on an HTTP thread")
    if "gui_settings_apply_key(scratch" not in setter:
        return fail("gui_net.c: server_handle_set() no longer dry-runs the value on a scratch copy")

    commands = body_of("gui_net_poll_commands", net_c)
    if "gui_ui_apply_remote_setting(" not in commands or "server_publish(" not in commands:
        return fail("gui_net.c: gui_net_poll_commands() must apply queued /set requests through "
                    "gui_ui_apply_remote_setting() and publish the settings for the HTTP threads")
    if "net_cmd_record_confirm" not in commands or "gui_record_resolve_pending(" not in commands:
        return fail("gui_net.c: gui_net_poll_commands() must resolve a client's /record?confirm= answer "
                    "through gui_record_resolve_pending() on the main thread")

    stats = body_of("server_build_stats", net_c)
    for key in ("rec_elapsed_ms", "rec_bytes", "rec_raw_a", "rec_comp_b", "rec_drops",
                "disk_free", "rec_pending", "rec_pending_text", "rec_finalizing", "status",
                "status_seq", "generation"):
        if f'\\"{key}\\"' not in stats:
            return fail(f"gui_net.c: /stats no longer carries {key}")
    if "published_status" not in stats or "gui_settings_json_escape(" not in stats:
        return fail("gui_net.c: /stats must relay the published (escaped) status line")
    if "published_pending" not in stats:
        return fail("gui_net.c: /stats must relay the published overwrite-prompt text, not read the record module")
    publish = body_of("server_publish", net_c)
    if "gui_record_pending_message(" not in publish or "published_pending" not in publish:
        return fail("gui_net.c: server_publish() must copy gui_record_pending_message() for the HTTP threads")

    a = net_c.find("static void client_apply_stats(")
    b = net_c.find("static int client_discovery_thread(")
    if a < 0 or b < 0 or b < a:
        return fail("gui_net.c: the client worker span was not found")
    worker = net_c[a:b]
    if re.search(r"app->settings\.\w+(\[[^\]]*\])*\s*=[^=]", worker):
        return fail("gui_net.c: the client worker writes app->settings; it must stage the snapshot for the main thread")
    if re.search(r"app->user_capture_mode_misrc\s*=", worker):
        return fail("gui_net.c: the client worker writes user_capture_mode_misrc off the main thread")
    if "gui_settings_save(" in worker:
        return fail("gui_net.c: the client worker saves settings")
    if 'client_get(cli->host, cli->port, "/settings"' not in worker:
        return fail("gui_net.c: the client worker does not poll /settings")
    if '"/record?confirm=%d"' not in worker or "rec_pending_text" not in worker:
        return fail("gui_net.c: the client worker must forward /record?confirm= and parse rec_pending_text")

    mirror = body_of("gui_net_poll_mirror", net_c)
    if "gui_settings_save(" in mirror:
        return fail("gui_net.c: gui_net_poll_mirror() saves settings")
    if "net_peer_state" not in mirror or "net_peer_status" not in mirror:
        return fail("gui_net.c: gui_net_poll_mirror() must wire net_peer_state and net_peer_status")
    if re.search(r"app->status_message\s*[\[=]", mirror) or "gui_app_set_status(" in mirror:
        return fail("gui_net.c: gui_net_poll_mirror() writes the bottom status bar; the relay goes to net_peer_status")
    if "client_mirror_overwrite_prompt(" not in mirror:
        return fail("gui_net.c: gui_net_poll_mirror() must mirror the server's overwrite prompt")
    prompt = body_of("client_mirror_overwrite_prompt", net_c)
    for needed in ("peer_rec_pending", "gui_popup_confirm(", "gui_popup_get_result(", "gui_popup_dismiss(",
                   "net_cmd_record_confirm_value", "net_cmd_record_confirm,"):
        if needed not in prompt:
            return fail(f"gui_net.c: client_mirror_overwrite_prompt() must use {needed}")
    if "gui_record_" in prompt:
        return fail("gui_net.c: the client's prompt mirror must not touch the local record module")
    serve = body_of("gui_net_serve_main", net_c)
    if "gui_record_check_popup(" not in serve:
        return fail("gui_net.c: the --net-serve loop must call gui_record_check_popup() or a confirm never resolves")
    if re.search(r"app->is_recording\s*=", net_c):
        return fail("gui_net.c sets is_recording; a client must never (gui_app_effective_recording is the readout)")

    view_end = body_of("gui_net_client_view_end", net_c)
    if "gui_settings_copy_fields(&app->settings, &s_peer_view, true)" not in view_end or \
       "gui_settings_save(&app->settings)" not in view_end:
        return fail("gui_net.c: gui_net_client_view_end() must restore the client's own settings "
                    "(client-local fields from the view) before it saves anything")

    extract = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/processing/gui_extract.c"))
    if "capture_ab_swap_invert" not in extract or "settings.misrc_v15_v25_ab_swap" in extract:
        return fail("gui_extract.c must read capture_ab_swap_invert (main-thread flag), not the settings field")

    main_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    usage = body_of("print_usage", main_c)
    for flag in ("--net-serve", "--net-client-probe"):
        if flag not in usage:
            return fail(f"misrc_gui.c: print_usage() does not list {flag}")
        pos = main_c.find(f'strcmp(argv[i], "{flag}") == 0')
        # The call, not a comment that names it (upstream's window-class
        # define near the top says "See the InitWindow() call").
        init = strip_c_comments(main_c).find("InitWindow(")
        if pos < 0 or init < 0 or pos > init:
            return fail(f"misrc_gui.c: {flag} must be dispatched before InitWindow")
    if "gui_net_client_view_begin(&app)" not in main_c or "gui_net_client_view_end(&app)" not in main_c:
        return fail("misrc_gui.c: the main loop must wrap the UI pass in gui_net_client_view_begin/end")
    if main_c.find("gui_net_client_view_end(&app)") < main_c.find("EndDrawing();"):
        return fail("misrc_gui.c: gui_net_client_view_end() must run after EndDrawing(): Clay draws text from pointers into app->settings")
    return 0


def check_cxadc_skips_card_b_when_rf_b_off(repo_root: Path) -> int:
    """RF B off at capture start leaves CXADC card 1 closed: no sysfs tenbit
    write, no open (which enables the card's IRQs and DMA), no read in the RF
    thread. card_count still means "cards present" so a two-card Clockgen rig
    keeps its forced 40/20 rate; only read_b decides whether card 1 is used.
    The capability flag follows, so extraction, the display thread and net
    clients all treat B as absent.

    RF B can be toggled while connected: the main thread opens or closes card
    1 once per frame (window loop and --net-serve) and hands it to the RF
    thread with a two-flag handshake. The RF thread raises b_active before it
    reads b_enabled; the main thread drops b_enabled before it reads
    b_active. Any other order lets a card close under a blocking read().
    Extraction re-selects its unpack function when B joins or leaves."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        cx = strip_c_comments(read_text(base / "input/gui_cxadc.c"))
        cap = strip_c_comments(read_text(base / "input/gui_capture.c"))
        disp = strip_c_comments(read_text(base / "processing/gui_display_thread.c"))
        net = strip_c_comments(read_text(base / "net/gui_net.c"))
        ext = strip_c_comments(read_text(base / "processing/gui_extract.c"))
        main_loop = strip_c_comments(read_text(base / "core/misrc_gui.c"))
    except OSError as exc:
        return fail(f"card-B skip guard: cannot read a source file: {exc}")
    try:
        start = extract_function_body(cx, "int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)")
        thread = extract_function_body(cx, "static int cxadc_capture_thread(void *ctx_ptr)")
        sync = extract_function_body(cx, "int gui_cxadc_sync_card_b(gui_app_t *app, bool want)")
    except RuntimeError as exc:
        return fail(f"gui_cxadc.c: {exc}")
    if not re.search(r"read_b\s*=\s*\(card_count\s*>\s*1\)\s*&&\s*app->settings\.capture_b", start):
        return fail("gui_cxadc.c: gui_cxadc_start() no longer derives read_b from card_count > 1 && settings.capture_b")
    if "cxadc_apply_tenbit_modes(app, open_count" not in start or "cxadc_open_cards(&s_cxadc, open_count)" not in start:
        return fail("gui_cxadc.c: gui_cxadc_start() programs or opens cards by card_count instead of open_count; "
                    "RF B off would open card 1 again")
    raise_active = thread.find("atomic_store(&ctx->b_active, true)")
    load_enabled = thread.find("atomic_load(&ctx->b_enabled)")
    card1_read = thread.find("card_fds[1]")
    if raise_active < 0 or load_enabled < 0 or not (raise_active < load_enabled < card1_read):
        return fail("gui_cxadc.c: cxadc_capture_thread() must raise b_active, then read b_enabled, "
                    "before it reads card 1")
    if thread.count("if (read_b)") < 2:
        return fail("gui_cxadc.c: cxadc_capture_thread() must gate the card 1 read and the B decode on read_b")
    drop_enabled = sync.find("atomic_store(&s_cxadc.b_enabled, false)")
    load_active = sync.find("atomic_load(&s_cxadc.b_active)")
    if drop_enabled < 0 or load_active < 0 or drop_enabled > load_active:
        return fail("gui_cxadc.c: gui_cxadc_sync_card_b() must drop b_enabled before it checks b_active")
    if not re.search(r"!enabled\s*&&\s*!atomic_load\(&s_cxadc\.b_active\)\s*&&\s*cxadc_card_b_is_open", sync):
        return fail("gui_cxadc.c: gui_cxadc_sync_card_b() may close card 1 only when b_enabled and b_active are both down")
    for where, text in (("core/misrc_gui.c", main_loop), ("net/gui_net.c", net)):
        if "gui_capture_service_channel_b(" not in text:
            return fail(f"{where}: the main loop no longer services RF B (gui_capture_service_channel_b)")
    if not re.search(r"want_ab\s*!=\s*s_extract_fn_ab", ext):
        return fail("gui_extract.c: extraction no longer re-selects its unpack function when channel B joins or leaves")
    if not re.search(r"capture_has_channel_b\s*=\s*\(cxadc_cards\s*>\s*1\)\s*&&\s*app->settings\.capture_b", cap):
        return fail("gui_capture.c: the CXADC start no longer clears capture_has_channel_b when RF B is off")
    if "app->capture_has_channel_b" not in disp:
        return fail("gui_display_thread.c: the display thread no longer skips channel B panels when the capture has no B")
    if not re.search(r"s_b_present\s*=\s*s_extract_fn_ab\s*&&", ext):
        return fail("gui_extract.c: s_b_present can turn true while the extract function is A-only "
                    "(s_buf_b would carry uninitialized data)")
    ingest = net[net.find("app->capture_backend_upstream = false;"):]
    if not re.match(r"app->capture_backend_upstream = false;\s*app->capture_has_channel_b = true;", ingest):
        return fail("gui_net.c: client ingest must start extraction with channel B present (the A+B unpack)")
    if '\\"has_channel_b\\"' not in net or 'json_bool(stats, "has_channel_b", true)' not in net:
        return fail("gui_net.c: /stats no longer carries has_channel_b, or the client no longer reads it (default true)")
    return 0


def check_pane_menu_contract(repo_root: Path) -> int:
    """Each of the four channel panes has its own data source and a right-click
    menu (view, source, row layout). The menu floats over waveform panes, whose
    click handler grabs the trigger level on any press inside them, so its
    click handler must run before the gear's and before the panel hit tests.
    Panel processing must pick samples by the pane's source, not by its row."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        ui = strip_c_comments(read_text(base / "ui/gui_ui.c"))
        registry = strip_c_comments(read_text(base / "visualization/panel_registry.c"))
    except OSError as exc:
        return fail(f"pane menu guard: cannot read a source file: {exc}")
    try:
        interactions = extract_function_body(ui, "void gui_handle_interactions(gui_app_t *app)")
        process = extract_function_body(registry, "static void process_config_panels(channel_panel_config_t *config,\n"
                                                  "                                  const int16_t *samples_a,\n"
                                                  "                                  const int16_t *samples_b,\n"
                                                  "                                  size_t count, uint32_t sample_rate)")
    except RuntimeError as exc:
        return fail(f"pane menu guard: {exc}")
    menu = interactions.find("gui_ui_handle_pane_menu_click(app)")
    gear = interactions.find("gui_ui_handle_channel_gear_click(app)")
    panels = interactions.find("gui_dropdown_handle_click(app)")
    if menu < 0 or gear < 0 or panels < 0:
        return fail("gui_ui.c: gui_handle_interactions() no longer calls the pane menu, gear and dropdown click handlers")
    if not (menu < gear < panels):
        return fail("gui_ui.c: the pane menu click handler must run before the gear and the panel hit tests "
                    "(a click on a menu option would also grab the trigger level)")
    if "MOUSE_BUTTON_RIGHT" not in interactions or "gui_ui_handle_pane_right_click(app)" not in interactions:
        return fail("gui_ui.c: a right press no longer opens the pane menu")
    if "left_source" not in process or "right_source" not in process:
        return fail("panel_registry.c: process_config_panels() no longer feeds each pane from its own source")
    return 0


def check_panel_source_harness_post_build(gui_path: Path) -> int:
    """Build and run the per-pane source harness in the GUI's meson build
    directory. CI runs this suite, not meson test, and compiles only the
    named product targets, so the harness target is compiled here."""
    build_dir = gui_path.parent
    if not (build_dir / "build.ninja").exists():
        print("SKIP: panel source harness (not a meson build directory)")
        return 0
    meson = shutil.which("meson")
    if meson is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("meson is required to build the panel source harness")
        print("SKIP: panel source harness (meson not available)")
        return 0
    built = subprocess.run([meson, "compile", "-C", str(build_dir), "gui_panel_source_test"],
                           capture_output=True, text=True)
    if built.returncode != 0:
        return fail("panel source harness failed to build:\n"
                    f"{built.stdout.strip()[-2000:]}\n{built.stderr.strip()[-2000:]}")
    exe = build_dir / ("gui_panel_source_test.exe" if gui_path.suffix == ".exe" else "gui_panel_source_test")
    if not exe.exists():
        return fail(f"panel source harness was not built: {exe}")
    ran = subprocess.run([str(exe)], capture_output=True, text=True)
    if ran.returncode != 0:
        return fail(f"panel source harness failed:\n{ran.stdout.strip()}\n{ran.stderr.strip()}")
    print(ran.stdout.strip())
    return 0


def check_channel_gear_clearance(repo_root: Path) -> int:
    """Each row's gear button floats in its top-left panel right after that
    panel's title (panel_gear_offset_x), and the waveform leaves
    PANEL_GEAR_SLOT after "CH A"/"CH B" before its time/div label and before
    any overlay button. A fixed offset put the gear on top of time/div on
    wide panels. test/gui_waveform_overlay_harness.c checks the geometry
    (meson test gui_waveform_overlay); this pins the wiring."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        ui = strip_c_comments(read_text(base / "ui/gui_ui.c"))
        osc = strip_c_comments(read_text(base / "visualization/gui_oscilloscope.c"))
        harness = read_text(repo_root / "misrc_tools/test/gui_waveform_overlay_harness.c")
    except OSError as exc:
        return fail(f"channel gear guard: cannot read a source file: {exc}")
    try:
        gear = extract_function_body(ui, "static void render_channel_gear(gui_app_t *app, int channel)")
    except RuntimeError as exc:
        return fail(f"gui_ui.c: {exc}")
    if "panel_gear_offset_x(" not in gear or ".offset = { gear_x, PANEL_GEAR_Y }" not in gear:
        return fail("gui_ui.c: render_channel_gear() no longer places the gear after the top-left panel's title")
    if not re.search(r"int div_x = ch_label_x \+ ch_label_w \+ PANEL_GEAR_SLOT \+ 8;", osc):
        return fail("gui_oscilloscope.c: time/div no longer leaves room for the channel gear after the label")
    if osc.count("channel_label_w + PANEL_GEAR_SLOT + 8") < 3:
        return fail("gui_oscilloscope.c: the Mode/Trig wrap and the Scale row no longer clear the channel gear")
    if "test_gear_clearance(&state);" not in harness:
        return fail("gui_waveform_overlay_harness.c: the gear clearance test is no longer run")

    # Clay_Raylib_Render wraps the pass in one rlScalef(ui_scale): panel bounds,
    # gui_ui_get_mouse_position() and gui_text_measure() share those units. The
    # overlay used to divide measured widths by the scale, which at 150% put
    # "Scale:"/"Mode:"/"Trig:" under their own buttons and the gear over "CH A".
    if "to_logical_w" in osc or re.search(r"1\.0f\s*/\s*gui_ui_get_scale_factor\(\)", osc):
        return fail("gui_oscilloscope.c: overlay geometry divides measured text by the UI scale again; "
                    "bounds, the mouse and text measurements are already in the same units")
    for test in ("test_scale_independence(&state);", "test_prefix_labels_clear_buttons(&state);"):
        if test not in harness:
            return fail(f"gui_waveform_overlay_harness.c: {test.split('(')[0]} is no longer run")
    return 0


def check_desktop_sized_cursor(repo_root: Path) -> int:
    """Both windows name the arrow cursor right after InitWindow. With raylib's
    default (no cursor) the X11 window inherits XWayland's root cursor, drawn
    at 1x: half the size of every other app on a 2x desktop. GLFW's standard
    cursor loads the theme at Xcursor.size instead."""
    base = repo_root / "misrc_tools/misrc_gui"
    for rel in ("core/misrc_gui.c", "input/gui_preview_v4l2.c"):
        try:
            src = strip_c_comments(read_text(base / rel))
        except OSError as exc:
            return fail(f"desktop-sized cursor guard: cannot read {rel}: {exc}")
        init = src.find("InitWindow(")
        if init < 0:
            return fail(f"{rel}: InitWindow( not found; the cursor guard needs updating")
        cursor = src.find("SetMouseCursor(MOUSE_CURSOR_ARROW);", init)
        if cursor < 0:
            return fail(f"{rel}: no SetMouseCursor(MOUSE_CURSOR_ARROW) after InitWindow; "
                        "the window falls back to the 1x X11 root cursor under XWayland")
        if src.count("SetMouseCursor(") != 1:
            return fail(f"{rel}: SetMouseCursor must be called exactly once "
                        "(raylib allocates a new GLFW cursor per call)")
    return 0


def check_ui_zoom_is_desktop_relative(repo_root: Path) -> int:
    """The UI zoom multiplies the desktop's own scale, so 100% is the size of
    every other app. It used to multiply physical pixels: on wm's 2x desktop
    100% was half size, and every zoom switched following off, which is how
    the live settings sat at 150% (three quarters) for weeks.
    gui_ui_scale_harness.c covers the arithmetic; this pins the wiring."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        gui_c = strip_c_comments(read_text(base / "core/misrc_gui.c"))
        ui_c = strip_c_comments(read_text(base / "ui/gui_ui.c"))
        table_c = strip_c_comments(read_text(base / "core/gui_settings_table.c"))
    except OSError as exc:
        return fail(f"desktop-relative zoom guard: cannot read a source file: {exc}")

    for name, src in (("misrc_gui.c", gui_c), ("gui_ui.c", ui_c)):
        if re.search(r"ui_scale_auto\s*=\s*false", src):
            return fail(f"{name}: something sets ui_scale_auto = false again; the zoom is relative "
                        "to the desktop, so zooming must never stop following it")
    if "VersionInfoUiScaleMatch" in ui_c:
        return fail("gui_ui.c: the Match now button is back; Follow desktop already applies the desktop scale")

    init = gui_c.find("InitWindow(")
    apply_after_init = gui_c.find("gui_ui_apply_ui_scale(&app.settings);", init)
    detect_after_init = gui_c.find("gui_ui_set_desktop_scale_percent(", init)
    if init < 0 or apply_after_init < 0 or detect_after_init < 0 or detect_after_init > apply_after_init:
        return fail("misrc_gui.c: after InitWindow the desktop scale must be detected, then applied "
                    "with gui_ui_apply_ui_scale(&app.settings), before the window is sized")
    if gui_c.count("gui_ui_apply_ui_scale(&app.settings);") < 3:
        return fail("misrc_gui.c: startup, the zoom shortcuts and desktop following must each apply "
                    "the scale through gui_ui_apply_ui_scale")
    if "gui_ui_zoom_step_percent(app->settings.ui_zoom_percent" not in ui_c:
        return fail("gui_ui.c: the Settings stepper no longer steps ui_zoom_percent")

    try:
        apply_body = extract_function_body(ui_c, "void gui_ui_apply_ui_scale(gui_settings_t *settings)")
    except RuntimeError as exc:
        return fail(f"gui_ui.c: {exc}")
    for snippet, why in (
        ("gui_ui_scale_effective_percent(", "the effective scale is desktop x zoom"),
        ("settings->ui_scale_auto ?", "Follow desktop off counts the desktop as 1x"),
        ("settings->ui_scale_percent = gui_ui_scale_legacy_percent(", "ui_scale_percent is written on the old grid for rollback"),
    ):
        if snippet not in apply_body:
            return fail(f"gui_ui.c: gui_ui_apply_ui_scale lost '{snippet}' ({why})")

    if not re.search(r"if \(settings->ui_zoom_percent == 0\) \{\s*settings->ui_zoom_percent = GUI_UI_ZOOM_DEFAULT_PERCENT;\s*"
                     r"settings->ui_scale_auto = true;", table_c):
        return fail("gui_settings_table.c: gui_settings_post_load no longer starts a pre-zoom file at "
                    "100% of the desktop with Follow desktop on")
    return 0


def check_about_dialog_never_scrolls_sideways(repo_root: Path) -> int:
    """The About/settings window clips vertically only. Clay does not compress
    children along a clipped axis (clay.h, "don't compress children"), so a
    horizontal clip left every hint at its one-line width: the fork's rows
    overflowed the 680 max and a drag or tilt-wheel scrolled the window
    sideways, cutting off the label column."""
    try:
        ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))
        body = extract_function_body(ui, "static void render_version_info_window(gui_app_t *app)")
    except (OSError, RuntimeError) as exc:
        return fail(f"About dialog guard: {exc}")
    start = body.find('CLAY_ID("VersionInfoWindow")')
    end = body.find('CLAY_ID("VersionInfoHeader")', start)
    if start < 0 or end < 0:
        return fail("gui_ui.c: VersionInfoWindow/VersionInfoHeader not found; the About dialog guard needs updating")
    window_config = body[start:end]
    if ".vertical = true" not in window_config:
        return fail("gui_ui.c: VersionInfoWindow no longer clips (and scrolls) vertically")
    if re.search(r"\.horizontal\s*=\s*true", window_config):
        return fail("gui_ui.c: VersionInfoWindow clips horizontally again; Clay then never wraps its "
                    "hints and the window scrolls sideways over the label column")
    return 0


def check_record_parity(repo_root: Path) -> int:
    """The Record button, its click and the R/Space keys read the effective
    recording and capture state (the server's on a net client that records on
    the server), and nothing outside gui_record.c ever assigns is_recording.
    A client that records the server's feed on this machine
    (net_client_record_local) queues the start/stop and runs it after
    gui_net_client_view_end(), where app->settings is its own: during the UI
    pass it is the server's copy and would name the files from the server's
    output folder."""
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))
    render_pos = ui.find('CLAY(CLAY_ID("RecordButton")')
    if render_pos < 0:
        return fail("gui_ui.c: the RecordButton render was not found")
    render_window = ui[max(0, render_pos - 2600):render_pos]
    if "gui_app_effective_recording(app)" not in render_window or "gui_app_control_capturing(app)" not in render_window:
        return fail("gui_ui.c: the RecordButton render does not use gui_app_effective_recording / gui_app_control_capturing")
    if "app->is_recording" in render_window:
        return fail("gui_ui.c: the RecordButton render still reads app->is_recording")
    click_pos = ui.find('Clay_PointerOver(CLAY_ID("RecordButton"))')
    if click_pos < 0:
        return fail("gui_ui.c: the RecordButton click was not found")
    click_window = ui[click_pos:click_pos + 1400]
    if "gui_app_effective_recording(app)" not in click_window or "gui_app_control_capturing(app)" not in click_window:
        return fail("gui_ui.c: the RecordButton click does not branch on the effective state")
    if "app->is_recording" in click_window:
        return fail("gui_ui.c: the RecordButton click still reads app->is_recording")
    # No longer static: the USB Reference Video dialog asks the same question.
    lock = ui[ui.find("bool gui_ui_settings_locked("):]
    lock = lock[:lock.find("\n}\n")]
    if "gui_net_client_peer_recording(app)" not in lock:
        return fail("gui_ui.c: gui_ui_settings_locked() does not consider the server's recording state")

    main_c = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"))
    key_r = main_c.find("IsKeyPressed(KEY_R)")
    key_space = main_c.find("IsKeyPressed(KEY_SPACE)")
    if key_r < 0 or key_space < 0:
        return fail("misrc_gui.c: the R / Space hotkeys were not found")
    if "gui_app_effective_recording(&app)" not in main_c[key_r:key_r + 500] or \
       "gui_app_control_capturing(&app)" not in main_c[key_r - 20:key_r + 500]:
        return fail("misrc_gui.c: the R hotkey does not use the effective recording and capture state")
    if "gui_app_control_capturing(&app)" not in main_c[key_space:key_space + 300]:
        return fail("misrc_gui.c: the Space hotkey does not use the effective capture state")

    capture_c = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/input/gui_capture.c"))
    try:
        start_body = extract_function_body(capture_c, "int gui_app_start_recording(gui_app_t *app)")
        stop_body = extract_function_body(capture_c, "void gui_app_stop_recording(gui_app_t *app)")
        service_body = extract_function_body(capture_c, "void gui_app_service_local_record_request(gui_app_t *app)")
    except RuntimeError as exc:
        return fail(f"gui_capture.c: {exc} (a net client's two record targets)")
    if "gui_net_client_request_record(app, true)" not in start_body or \
       "gui_net_client_request_record(app, false)" not in stop_body:
        return fail("gui_capture.c: a client that records on the server must still forward record on/off to it")
    if "net_local_record_request" not in start_body or "net_local_record_request" not in stop_body:
        return fail("gui_capture.c: a client that records locally must QUEUE the start/stop "
                    "(net_local_record_request): during the UI pass app->settings is the server's copy")
    if "gui_record_start(app)" not in service_body or "gui_record_stop(app)" not in service_body:
        return fail("gui_capture.c: gui_app_service_local_record_request() must run the queued "
                    "gui_record_start / gui_record_stop")
    view_end_pos = main_c.find("gui_net_client_view_end(&app);")
    service_pos = main_c.find("gui_app_service_local_record_request(&app);")
    if view_end_pos < 0 or service_pos < view_end_pos:
        return fail("misrc_gui.c: gui_app_service_local_record_request() must run after "
                    "gui_net_client_view_end(), where app->settings is the client's own")
    if main_c.count("gui_app_service_local_record_request(&app);") < 2:
        return fail("misrc_gui.c: the exit path must service the queued local stop too, "
                    "or a client's local recording is never finalized")
    if "app->is_recording ||" not in lock:
        return fail("gui_ui.c: gui_ui_settings_locked() does not consider a client's local recording")

    gui_root = repo_root / "misrc_tools/misrc_gui"
    for path in sorted(gui_root.rglob("*.c")):
        if path.name == "gui_record.c":
            continue
        src = strip_c_comments(read_text(path))
        m = re.search(r"(->|\.)is_recording\s*=[^=]", src)
        if m:
            line = src[:m.start()].count("\n") + 1
            return fail(f"{path.relative_to(repo_root)}:{line} assigns is_recording; only gui_record.c may")
    return 0


def check_net_query_runtime(repo_root: Path) -> int:
    """Compiles misrc_gui/net/gui_net_query.c standalone and runs the query,
    percent-decode and percent-encode cases the settings setter depends on: a
    value that decodes wrongly lands in a setting."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: net query runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the net query runtime guard")
        print("SKIP: net query runtime guard (cc not available)")
        return 0
    harness_path = repo_root / "misrc_tools/test/net_query_harness.c"
    query_path = repo_root / "misrc_tools/misrc_gui/net/gui_net_query.c"
    for required in (harness_path, query_path):
        if not required.exists():
            return fail(f"Net query guard source is missing: {required}")
    with tempfile.TemporaryDirectory(prefix="misrc_net_query_guard_") as temp_root:
        exe_path = Path(temp_root) / "net_query_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            f"-I{query_path.parent}",
            str(harness_path),
            str(query_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Net query harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Net query harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_url_open_is_whitelisted(repo_root: Path) -> int:
    """raylib's OpenURL() builds a shell command and runs it through system(),
    rejecting only the single quote. Ctrl+click on a reader URL reaches it, and in
    LAN mode part of that URL is gethostname(), with reader_host a public field a
    future caller could wire to a hand-edited settings string. So every call site
    must sit behind the whitelist rather than trusting one blacklisted character.
    A second, unguarded OpenURL() added later is the regression this exists to
    catch."""
    gui_root = repo_root / "misrc_tools"
    call_sites = []
    for path in sorted(gui_root.rglob("*.c")) + sorted(gui_root.rglob("*.h")):
        if ".deps" in path.parts or "build" in path.parts:
            continue
        code = strip_c_comments(read_text(path))
        for m in re.finditer(r"\bOpenURL\s*\(", code):
            call_sites.append((path, code[: m.start()].count("\n") + 1, code, m.start()))

    if not call_sites:
        return fail(
            "no OpenURL() call site found; the Ctrl+click-to-open affordance is gone "
            "and this guard is guarding nothing"
        )
    if len(call_sites) != 1:
        where = ", ".join(f"{p.name}:{n}" for p, n, _, _ in call_sites)
        return fail(
            f"expected exactly one OpenURL() call site, found {len(call_sites)}: {where}. "
            "Each one hands a string to a shell; route them all through "
            "rtsp_url_is_safe_to_open() and update this guard deliberately."
        )

    path, lineno, code, offset = call_sites[0]
    # The whitelist must be the branch condition guarding this call, not merely
    # present somewhere in the file.
    window = code[max(0, offset - 400):offset]
    if "rtsp_url_is_safe_to_open" not in window:
        return fail(
            f"{path.name}:{lineno}: OpenURL() is not guarded by rtsp_url_is_safe_to_open(). "
            "Unvalidated text reaching OpenURL() reaches system()."
        )

    checker = strip_c_comments(read_text(path))
    m = re.search(r"static bool rtsp_url_is_safe_to_open\(const char \*url\)\s*\{(.*?)\n\}",
                  checker, re.S)
    if not m:
        return fail(f"{path.name}: rtsp_url_is_safe_to_open() is missing or its shape changed")
    body = m.group(1)
    for required, why in (
        ('"rtsp://"', "must require a scheme it built"),
        ('"http://"', "must require a scheme it built"),
        ("return false", "must reject by default rather than allow by default"),
    ):
        if required not in body:
            return fail(f"rtsp_url_is_safe_to_open() {why}; missing {required}")
    # Enumerate what the whitelist actually permits, rather than hunting for
    # specific bad characters. Blacklisting misses what it was not told about --
    # an earlier version of this guard looked for "*p == '\''" and sailed past
    # the escaped form the compiler actually sees.
    ALLOWED_PUNCTUATION = set(".-_:/")
    permitted = re.findall(r"\*p == '(\\.|[^'])'", body)
    if not permitted:
        return fail(
            "rtsp_url_is_safe_to_open() no longer names the punctuation it allows; "
            "this guard can no longer tell a URL from a shell command"
        )
    for ch in permitted:
        if ch not in ALLOWED_PUNCTUATION:
            return fail(
                f"rtsp_url_is_safe_to_open() permits {ch!r} in a URL that OpenURL() "
                f"hands to system(); only {''.join(sorted(ALLOWED_PUNCTUATION))} are safe there"
            )
    # The alphanumeric ranges carry the rest of the hostname and path.
    for rng in ("'a'", "'z'", "'A'", "'Z'", "'0'", "'9'"):
        if rng not in body:
            return fail(f"rtsp_url_is_safe_to_open() no longer bounds its {rng} range")
    return 0


def check_live_stream_readout_cannot_resize_the_panel(repo_root: Path) -> int:
    """The settings panel is CLAY_SIZING_FIT, so it measures itself from its
    contents. A label that changes every frame therefore re-measures the whole
    panel, and since the panel is centre-attached it grows and shrinks around the
    middle -- the window visibly breathes while a stream runs.

    The fix is that the live readout sits in a FIXED box. This pins that, because
    the regression is invisible in a diff: dropping the wrapper leaves working,
    correct-looking code that just happens to make the window pulse."""
    # Moved to the USB Reference Video dialog with the rest of the stream UI.
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_usbref_settings.c"))

    m = re.search(r'CLAY\(CLAY_ID\("RtspLiveBox"\),\s*\{(.*?)\}\)\s*\{', ui, re.S)
    if not m:
        return fail(
            "the RtspLiveBox wrapper is gone; the live stream readout is free to "
            "resize the settings panel again"
        )
    # The WIDTH specifically. The height is always fixed, so searching the whole
    # sizing clause for CLAY_SIZING_FIXED passes even when the width has been
    # loosened to FIT -- which is the only axis that matters here, and which
    # mutation testing caught this check sailing past.
    sizing = re.search(r"\.sizing\s*=\s*\{\s*([^,]+),", m.group(1))
    if not sizing:
        return fail("RtspLiveBox no longer states a sizing clause this guard can read")
    width = sizing.group(1).strip()
    if not width.startswith("CLAY_SIZING_FIXED"):
        return fail(
            f"RtspLiveBox width is {width}, not CLAY_SIZING_FIXED. Its text width "
            "then feeds back into a CLAY_SIZING_FIT panel and the window breathes "
            "as the numbers change."
        )
    return 0


def check_stream_password_is_strong_and_unleaked(repo_root: Path) -> int:
    """The stream password guards a customer's tape on a shared network, so two
    things must hold and neither is visible in a diff.

    It must be unguessable: rand() is seeded from the clock, and anyone who can
    see the stream exists already knows roughly when it started, so a password
    from rand() is worth very little. It must come from the kernel's CSPRNG.

    And it must not leak. The reader URLs are shown on screen, copied to the
    clipboard and opened with Ctrl+click; the ffmpeg argv is readable from
    /proc/<pid>/cmdline by any local user and is echoed into its log. A password
    in either would defeat the point of having one."""
    stream = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/streaming/gui_rtsp_stream.c"))

    gen = re.search(r"static bool rs_make_password\(char \*out, size_t cap\)\s*\{(.*?)\n\}",
                    stream, re.S)
    if not gen:
        return fail("rs_make_password() is missing or its shape changed")
    body = gen.group(1)

    if "getrandom" not in body:
        return fail("rs_make_password() does not use getrandom(); the stream password "
                    "must come from the kernel CSPRNG")
    if "/dev/urandom" not in body:
        return fail("rs_make_password() has no /dev/urandom fallback for a kernel "
                    "without getrandom()")
    # Word-boundaried: a bare "random(" substring also matches getrandom(),
    # which is the one call we require.
    for banned in ("rand", "srand", "random", "drand48", "lrand48", "GetTime"):
        if re.search(rf"\b{banned}\s*\(", body):
            return fail(
                f"rs_make_password() uses {banned}(). A clock-seeded or otherwise "
                "predictable source makes the password guessable from roughly when "
                "the stream started, which anyone who can see it already knows."
            )

    # Enough of it to be worth having: 16 draws from a 32-symbol alphabet is 80 bits.
    alpha = re.search(r'alphabet\[\]\s*=\s*"([^"]+)"', body)
    if not alpha or len(set(alpha.group(1))) < 32:
        return fail("the stream password alphabet is smaller than 32 distinct symbols")
    draws = re.search(r"unsigned char raw\[(\d+)\]", body)
    if not draws or int(draws.group(1)) < 12:
        return fail("the stream password draws fewer than 12 symbols; too short to "
                    "resist an offline guess")

    # It must not reach the reader URLs...
    urls = re.search(r"static void rs_fill_urls\(const gui_rtsp_stream_opts_t \*opts\)\s*\{(.*?)\n\}",
                     stream, re.S)
    if not urls:
        return fail("rs_fill_urls() is missing or its shape changed")
    for leak in ("password", "read_password"):
        if leak in urls.group(1):
            return fail(
                "rs_fill_urls() mentions the password. Those URLs are shown on "
                "screen, copied to the clipboard and opened via the shell -- a "
                "credential in them is a credential published."
            )

    # ...nor the ffmpeg command line.
    argv = re.search(r"static void rs_build_argv\(char \*argv\[\],(.*?)\n\}", stream, re.S)
    if not argv:
        return fail("rs_build_argv() is missing or its shape changed")
    for leak in ("password", "read_password"):
        if leak in argv.group(1):
            return fail(
                "rs_build_argv() mentions the password. argv is readable from "
                "/proc/<pid>/cmdline by any local user and lands in ffmpeg's log."
            )
    return 0


def check_bitrate_stepper_survives_a_reload(repo_root: Path) -> int:
    """The codec window's bitrate stepper and gui_settings.c's parser have to
    agree on what a legal value is, and they are written five hundred lines apart
    in different files.

    If the stepper can reach a value the parser rejects, the parser quietly
    substitutes the default -- so the setting appears to work, survives until the
    app is restarted, and then is silently something else. That is the same
    failure check_rtsp_settings_roundtrip exists for, one level down: not a
    missing site, but two sites that disagree."""
    # The stepper moved to the USB Reference Video dialog with the rest of the
    # streaming UI; the clamp it must agree with is still in the settings table.
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_usbref_settings.c"))
    settings = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c"))

    bounds = {}
    for name in ("RTSP_BITRATE_MIN_KBPS", "RTSP_BITRATE_MAX_KBPS",
                 "RTSP_BITRATE_STEP_KBPS", "RTSP_BITRATE_DEFAULT_KBPS"):
        m = re.search(rf"#define\s+{name}\s+(\d+)", ui)
        if not m:
            return fail(f"gui_usbref_settings.c no longer defines {name}")
        bounds[name] = int(m.group(1))

    m = re.search(
        r"usbref_rtsp_bitrate_kbps\s*=\s*\(b == 0 \|\| \(b >= (\d+) && b <= (\d+)\)\)",
        settings)
    if not m:
        return fail("could not read the bitrate clamp in gui_settings_table.c; if its shape "
                    "changed, this guard must be updated with it")
    lo, hi = int(m.group(1)), int(m.group(2))

    if bounds["RTSP_BITRATE_MIN_KBPS"] < lo or bounds["RTSP_BITRATE_MAX_KBPS"] > hi:
        return fail(
            f"the bitrate stepper spans {bounds['RTSP_BITRATE_MIN_KBPS']}..."
            f"{bounds['RTSP_BITRATE_MAX_KBPS']} kbit/s but gui_settings.c only accepts "
            f"{lo}...{hi} on reload. A value outside that range is replaced by the "
            "default when the app restarts, so the setting appears to work and then "
            "silently is not what was chosen."
        )
    if bounds["RTSP_BITRATE_MIN_KBPS"] >= bounds["RTSP_BITRATE_MAX_KBPS"]:
        return fail("the bitrate stepper's minimum is not below its maximum")
    if bounds["RTSP_BITRATE_STEP_KBPS"] <= 0:
        return fail("the bitrate step is not positive; the stepper would not move")
    if not (bounds["RTSP_BITRATE_MIN_KBPS"] <= bounds["RTSP_BITRATE_DEFAULT_KBPS"]
            <= bounds["RTSP_BITRATE_MAX_KBPS"]):
        return fail(
            f"the default bitrate ({bounds['RTSP_BITRATE_DEFAULT_KBPS']}) is outside "
            "the stepper's own range, so the first click would jump rather than step"
        )
    return 0


def check_windows_packaging_assertions(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "test \"$(find dist -maxdepth 1 -type f | wc -l)\" -eq 1",
        "test \"$(find dist -maxdepth 1 -name '*.exe' | wc -l)\" -eq 1",
        "objdump -p dist/MISRC.exe",
        "test \"$(objdump -p dist/MISRC.exe | awk '/^Subsystem[[:space:]]/ {print $2; exit}')\" = \"00000002\"",
        "assert_no_nonsystem_dlls()",
        "assert_no_nonsystem_dlls \"dist/MISRC.exe\"",
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_x86.zip\"",
        "Compress-Archive -Path @(\"dist/MISRC.exe\")",
        "if ($zip.Entries.Count -ne 1)",
        "$entry.FullName.Contains('/') -or $entry.FullName.Contains('\\')",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Windows packaging assertion: {snippet}")
    return 0

def check_android_packaging_assertions(workflow_path: Path) -> int:
    """Assert the android-apk CI job verifies the APK the same way local build-apk.sh does:
    - apksigner verify (signature integrity)
    - aapt2 dump badging sdkVersion:'30' + targetSdkVersion:'34' + package dev.misrc.gui
    - lib/arm64-v8a/libmisrc_gui.so present in the APK
    - libmisrc_gui.so is ARM aarch64 (not x86_64 host leak)
    - no host /usr/lib/x86_64 or /usr/local/lib leaked into the cross .so
    These mirror the local verification in android/build-apk.sh + the manual
    checks performed during the android-support branch development."""
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "bash android/build-deps-android.sh",
        "bash android/gen-cross-file.sh",
        "meson setup --cross-file android/aarch64-linux-android.ini",
        "meson compile -C build-android misrc_gui",
        "test -f build-android/libmisrc_gui.so",
        "bash android/build-apk.sh",
        # The CI job invokes tools via $ANDROID_HOME variable-prefixed paths,
        # so assert on the tool-name substrings rather than bare "apksigner verify".
        "34.0.0/apksigner",
        "verify \"$APK\"",
        "34.0.0/aapt2",
        "dump badging",
        "sdkVersion:'30'",
        "targetSdkVersion:'34'",
        "package: name='dev.misrc.gui'",
        "lib/arm64-v8a/libmisrc_gui.so",
        "ARM aarch64",
        "/usr/lib/x86_64|/usr/local/lib",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required Android packaging assertion: {snippet}")
    return 0

def check_release_artifact_naming_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert every release artifact filename follows
    <Platform>_MISRC_GUI_<version>_<arch>.<ext> with the platform name capitalized
    (Linux, Windows, macOS, Android), and that the lowercase v1.1.7 malform
    cannot regress.

    Regression (v1.1.7): Linux + Android release assets shipped as
    linux_MISRC_dev-..._arm64.zip / android_MISRC_dev-..._arm64.apk under tag
    v1.1.7 (lowercase prefix AND dev-named). This guard forbids the lowercase
    platform prefixes on release artifacts so the naming half of the malform is
    caught; check_release_version_resolution_contract covers the dev-name half.
    """
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "workflow_dispatch:",
        "artifact_suffix: x86",
        "artifact_suffix: arm64",
        "APPIMAGE_NAME=\"Linux_MISRC_GUI_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.AppImage\"",
        "ZIP_NAME=\"Linux_MISRC_GUI_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.zip\"",
        "path: Linux_MISRC_GUI_*_${{ matrix.artifact_suffix }}.zip",
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_x86.zip\"",
        "path: Windows_MISRC_GUI_*_x86.zip",
        "$ZipPath = \"Windows_MISRC_GUI_${{ steps.version.outputs.version }}_arm64.zip\"",
        "path: Windows_MISRC_GUI_*_arm64.zip",
        "DMG_NAME=\"macOS_MISRC_GUI_${BUILD_VERSION}_universal.dmg\"",
        "path: macOS_MISRC_GUI_*_universal.dmg",
        "release-assets/**/Linux_MISRC_GUI_*_x86.zip",
        "release-assets/**/Linux_MISRC_GUI_*_arm64.zip",
        "release-assets/**/Windows_MISRC_GUI_*_x86.zip",
        "release-assets/**/Windows_MISRC_GUI_*_arm64.zip",
        "release-assets/**/macOS_MISRC_GUI_*_universal.dmg",
        "APK_ARM64=\"Android_MISRC_GUI_${TAG}_arm64.apk\"",
        "release-assets/**/Android_MISRC_GUI_*_arm64.apk",
    ]
    forbidden_snippets = [
        # Legacy pre-convention APK/shapes.
        "misrc_gui-${TAG}-android-arm64.apk",
        "release-assets/**/misrc_gui-*-android-arm64.apk",
        "misrc_gui-*-windows-x86_64.zip",
        "misrc_gui-*-macos-universal-app.tar.gz",
        "MISRC_*_windows_x86.zip",
        "MISRC_*_macos_universal.dmg",
        "MISRC_*_linux_x86.zip",
        "MISRC_*_linux_arm64.zip",
        "release-assets/**/misrc_gui-*-linux-x86_64.AppImage",
        "release-assets/**/misrc_gui-*-linux-arm64.AppImage",
        "release-assets/**/misrc_gui-*-windows-x86_64.zip",
        "release-assets/**/misrc_gui-*-macos-universal-app.tar.gz",
        "release-assets/**/MISRC_*_linux_x86.zip",
        "release-assets/**/MISRC_*_linux_arm64.zip",
        "release-assets/**/MISRC_*_windows_x86.zip",
        "release-assets/**/MISRC_*_macos_universal.dmg",
        "release-assets/**/Linux_MISRC_*_x86.zip",
        "release-assets/**/Linux_MISRC_*_arm64.zip",
        "release-assets/**/Windows_MISRC_*_x86.zip",
        "release-assets/**/Windows_MISRC_*_arm64.zip",
        "release-assets/**/macOS_MISRC_*_universal.dmg",
        "release-assets/**/Android_MISRC_*_arm64.apk",
        # Lowercase platform prefixes on release artifacts (v1.1.7 malform).
        "linux_MISRC_${BUILD_VERSION}",
        "windows_MISRC_${{ steps.version.outputs.version }}",
        "macos_MISRC_${BUILD_VERSION}",
        "android_MISRC_${TAG}",
        "release-assets/**/linux_MISRC_*_x86.zip",
        "release-assets/**/linux_MISRC_*_arm64.zip",
        "release-assets/**/windows_MISRC_*_x86.zip",
        "release-assets/**/windows_MISRC_*_arm64.zip",
        "release-assets/**/macos_MISRC_*_universal.dmg",
        "release-assets/**/android_MISRC_*_arm64.apk",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing required release artifact naming snippet: {snippet}")
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow still contains forbidden release artifact naming snippet: {snippet}")
    # The Android APK filename is produced by android/build-apk.sh (not the
    # workflow), so assert the capitalized convention there too.
    apk_script = repo_root / "android" / "build-apk.sh"
    if not apk_script.exists():
        return fail(f"android/build-apk.sh is missing: {apk_script}")
    apk_text = read_text(apk_script)
    if "Android_MISRC_GUI_${VERSION}_arm64.apk" not in apk_text:
        return fail(
            "android/build-apk.sh must name the APK Android_MISRC_GUI_${VERSION}_arm64.apk "
            "(capitalized platform prefix, matching the release convention)."
        )
    if "android_MISRC_${VERSION}_arm64.apk" in apk_text:
        return fail(
            "android/build-apk.sh still uses the lowercase android_MISRC_ prefix "
            "(v1.1.7 malform); use Android_MISRC_."
        )
    return 0


def check_release_download_link_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert the in-app updater's asset filename mapping matches the release
    artifact naming, and that the live link check is wired into CI.

    Regression (user report, release v1.2.4): gui_ui.c built asset names
    missing the "_GUI" infix (Windows_MISRC_%s_x86.zip) while the workflow
    publishes Windows_MISRC_GUI_<tag>_x86.zip, so every platform's
    update-check Download button 404'd for every release.
    check_release_artifact_naming_contract only pinned the WORKFLOW side;
    nothing cross-checked the APP side. This guard pins both sides to the
    identical naming and requires the live release_link_check.py CI test
    (HEAD-checks the exact URLs the app builds, against the latest
    published release on every run and against the just-published tag in
    the release job).
    """
    gui_c_path = repo_root / "misrc_tools" / "misrc_gui" / "ui" / "gui_ui.c"
    if not gui_c_path.exists():
        return fail(f"Missing gui_ui.c: {gui_c_path}")
    gui_text = read_text(gui_c_path)

    required_patterns = [
        "Android_MISRC_GUI_%s_arm64.apk",
        "macOS_MISRC_GUI_%s_universal.dmg",
        "Windows_MISRC_GUI_%s_arm64.zip",
        "Windows_MISRC_GUI_%s_x86.zip",
        "Linux_MISRC_GUI_%s_arm64.zip",
        "Linux_MISRC_GUI_%s_x86.zip",
    ]
    for pattern in required_patterns:
        if pattern not in gui_text:
            return fail(
                f"gui_ui.c release asset mapping is missing the exact pattern: {pattern} "
                "(must match the workflow's <Platform>_MISRC_GUI_<tag>_<arch>.<ext> naming)"
            )
    for pattern in [p.replace("MISRC_GUI_", "MISRC_") for p in required_patterns]:
        if pattern in gui_text:
            return fail(
                f"gui_ui.c still contains the broken asset mapping (missing the _GUI "
                f"infix) that 404'd the updater's Download button on every platform: {pattern}"
            )

    link_check = repo_root / "misrc_tools" / "test" / "release_link_check.py"
    if not link_check.exists():
        return fail(f"Missing release link check CI test: {link_check}")
    link_text = read_text(link_check)
    for snippet in [
        "releases/latest",
        "releases/download",
        "gui_ui.c",
        "gui_ui_build_release_asset_filename_for_platform",
        "--tag",
    ]:
        if snippet not in link_text:
            return fail(f"release_link_check.py is missing required snippet: {snippet}")

    workflow_text = read_text(workflow_path)
    for snippet in [
        "release-link-check:",
        "python3 misrc_tools/test/release_link_check.py",
        'release_link_check.py --tag "${{ steps.tag.outputs.tag }}"',
    ]:
        if snippet not in workflow_text:
            return fail(f"Workflow is missing release download link check wiring: {snippet}")
    return 0


def check_release_version_resolution_contract(repo_root: Path, workflow_path: Path) -> int:
    """Assert release-context CI runs resolve the tag (never a dev string) and
    that a release can never ship a dev-named artifact under the tag.

    Regression (v1.1.7): the Linux job received an empty release_tag input and
    silently fell through to git-version.sh on a --no-tags --depth=1 shallow
    clone (-> dev-2026-08-25-c991f39), and the android-apk job had no
    version-resolution step and never passed MISRC_TOOLS_VERSION_OVERRIDE to
    build-apk.sh (which calls git-version.sh directly -> always dev-...,
    versionCode 1). Both shipped dev-named assets under tag v1.1.7.

    Requires:
      - misrc_tools/ci-resolve-version.sh exists, is executable, and contains
        the empty-release_tag hard-fail + the release+dev hard-fail.
      - every build job (linux, windows-x86, windows-arm64, macos, android)
        calls the shared resolver.
      - every build job exports MISRC_TOOLS_VERSION_OVERRIDE from the version
        step (Android especially: build-apk.sh reads it via git-version.sh).
      - the release job has the pre-upload 'no dev leak / tag-named' assertion.
    """
    resolver = repo_root / "misrc_tools" / "ci-resolve-version.sh"
    if not resolver.exists():
        return fail(f"Missing shared release/version resolver: {resolver}")
    if not os.access(resolver, os.X_OK):
        return fail("misrc_tools/ci-resolve-version.sh must be executable (chmod +x)")
    rv = read_text(resolver)
    for snippet in [
        "refs/tags/",
        "workflow_dispatch",
        "CI_CREATE_RELEASE",
        "CI_RELEASE_TAG",
        "release_tag input is empty",
        "release run resolved a dev version",
    ]:
        if snippet not in rv:
            return fail(f"misrc_tools/ci-resolve-version.sh is missing required snippet: {snippet}")

    wf = read_text(workflow_path)
    for snippet in [
        "CI_EVENT_NAME: ${{ github.event_name }}",
        "CI_CREATE_RELEASE: ${{ github.event.inputs.create_release }}",
        "CI_RELEASE_TAG: ${{ github.event.inputs.release_tag }}",
    ]:
        if snippet not in wf:
            return fail(f"Workflow is missing release-version-resolution env snippet: {snippet}")

    # The resolver must be invoked by all 5 build jobs (linux, windows-x86,
    # windows-arm64, macos, android).
    call_count = wf.count("misrc_tools/ci-resolve-version.sh")
    if call_count < 5:
        return fail(
            f"ci-resolve-version.sh must be called by all 5 build jobs (linux, "
            f"windows-x86, windows-arm64, macos, android); found {call_count} call(s)."
        )

    # All 5 build jobs must export MISRC_TOOLS_VERSION_OVERRIDE from the version
    # step. Android is the critical one: build-apk.sh calls git-version.sh
    # directly, so without the override it always yields dev-... on the shallow
    # clone (the v1.1.7 APK regression).
    override_count = wf.count("MISRC_TOOLS_VERSION_OVERRIDE: ${{ steps.version.outputs.version }}")
    if override_count < 5:
        return fail(
            f"All 5 build jobs must export MISRC_TOOLS_VERSION_OVERRIDE from the "
            f"version step (Android was missing this in the v1.1.7 regression); "
            f"found {override_count}."
        )

    # The release job must have the pre-upload assertion.
    if "Assert release assets are tag-named" not in wf:
        return fail(
            "Release job is missing the pre-upload 'Assert release assets are "
            "tag-named (no dev leak)' step."
        )
    if "*_MISRC_GUI_dev-*" not in wf:
        return fail("Release pre-upload assertion must match '*_MISRC_GUI_dev-*' dev-named artifacts.")
    return 0


def check_ci_only_tier_contract(deploy_workflow_path: Path) -> int:
    """The CI-only tier must stay wired, and must stay fail-closed.

    A change under .github/ alone produces a bit-identical binary, so since 2026-09-08 it
    runs this guard suite instead of the four-machine build. Two ways that goes wrong
    silently: the `guards` job is dropped or renamed, leaving such a change with NO check
    at all; or the fail-closed arm is lost, so an unclassifiable change (dispatch, force
    push, API error) takes the narrow path and skips the build it needed.
    """
    if not deploy_workflow_path.exists():
        return fail(f"Fork deploy workflow is missing: {deploy_workflow_path}")
    text = read_text(deploy_workflow_path)
    required = [
        # The classifier emits the tier, and run_everything forces it off.
        "ci_only=true",
        "ci_only=false",
        # The job that IS the tier's only check.
        "  guards:",
        "ci_guard_tests.py --static-only",
        "needs.changes.outputs.ci_only == 'true'",
        # The build and the tag bump must both stand down for it.
        "needs.changes.outputs.ci_only != 'true'",
    ]
    for snippet in required:
        if snippet not in text:
            return fail(f"CI-only tier contract is missing required snippet: {snippet}")
    # run_everything is the fail-closed path. Slice to the closing brace at its own
    # indentation, not the first "}" -- the body is full of "${GITHUB_OUTPUT}".
    body = text[text.index("run_everything() {") :]
    run_everything = body[: body.index("\n          }")]
    if 'echo "ci_only=false"' not in run_everything:
        return fail(
            "run_everything must force ci_only=false: an unclassifiable change may "
            "never take the narrow path and skip the build"
        )
    return 0


def check_build_workflow_entrypoint_contract(build_workflow_path: Path) -> int:
    if not build_workflow_path.exists():
        return fail(f"Build workflow entrypoint is missing: {build_workflow_path}")
    workflow_text = read_text(build_workflow_path)
    required_snippets = [
        "name: Build and release binary",
        "workflow_dispatch:",
        "create_release:",
        "release_tag:",
        "push:",
        "- 'v*'",
        "preflight-guard-tests:",
        "linux-appimage:",
        "windows-exe:",
        "macos-app-universal:",
        "android-apk:",
        "release:",
    ]
    for snippet in required_snippets:
        if snippet not in workflow_text:
            return fail(f"Build workflow entrypoint is missing required snippet: {snippet}")
    forbidden_snippets = [
        "uses: ./.github/workflows/release-sanity-build.yml",
    ]
    for snippet in forbidden_snippets:
        if snippet in workflow_text:
            return fail(f"Build workflow entrypoint still contains legacy reusable wrapper snippet: {snippet}")
    return 0


def check_no_capture_stability_clutter(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    forbidden_workflow_snippets = [
        "bash misrc_tools/test/capture_stability_ci.sh",
        "capture-stability-${{ matrix.arch }}",
        "capture-stability-linux-${{ matrix.arch }}",
        "capture-stability-linux-x86_64",
        "capture-stability-linux-arm64",
        "capture-stability-windows",
        "capture-stability-macos-universal",
        "Upload capture stability logs",
        "Upload macOS capture stability logs",
        "Run capture stability loops",
    ]
    for snippet in forbidden_workflow_snippets:
        if snippet in workflow_text:
            return fail(f"Workflow still contains capture-stability Actions clutter snippet: {snippet}")
    return 0
def check_local_build_bootstrap_contract(repo_root: Path,
                                         dev_notes_path: Path,
                                         installation_md_path: Path) -> int:
    script_path = repo_root / "scripts/build-local.ps1"
    if not script_path.exists():
        return fail(f"Missing Windows local-build bootstrap script: {script_path}")
    script_text = read_text(script_path)
    required_script_snippets = [
        "param(",
        "BootstrapOnly",
        "NoAutoInstall",
        "python -m mesonbuild.mesonmain --version",
        "python -m pip install --user --upgrade meson ninja",
        "Add-PythonUserScriptsToPath",
    ]
    for snippet in required_script_snippets:
        if snippet not in script_text:
            return fail(f"build-local.ps1 is missing required bootstrap snippet: {snippet}")

    if not dev_notes_path.exists():
        return fail(f"Missing dev notes file for bootstrap contract: {dev_notes_path}")
    dev_notes_text = read_text(dev_notes_path)
    required_dev_notes_snippets = [
        "Windows local build bootstrap note",
        "pwsh -File scripts/build-local.ps1 -BootstrapOnly",
        "pwsh -File scripts/build-local.ps1",
        "python misrc_tools/test/ci_guard_tests.py --static-only",
    ]
    for snippet in required_dev_notes_snippets:
        if snippet not in dev_notes_text:
            return fail(f"Dev notes are missing local build bootstrap guidance: {snippet}")

    # The build/installation snippets live in INSTALLATION.md at the repo root
    # (the main README.md is app-use-only). misrc_tools/README.md points here.
    if not installation_md_path.exists():
        return fail(f"Missing INSTALLATION.md for bootstrap contract: {installation_md_path}")
    installation_text = read_text(installation_md_path)
    required_installation_snippets = [
        "scripts/build-local.ps1 -BootstrapOnly",
        "scripts/build-local.ps1",
    ]
    for snippet in required_installation_snippets:
        if snippet not in installation_text:
            return fail(f"INSTALLATION.md is missing local-build bootstrap snippet: {snippet}")
    return 0


def check_local_deps_cache_contract(repo_root: Path,
                                     workflow_path: Path,
                                     dev_notes_path: Path,
                                     tools_readme_path: Path) -> int:
    """Assert the local==CI deps caching path is present and wired.

    Prevents silent removal of the deps scripts, stamp gate, build-local
    auto-invocation, CI actions/cache, or docs that describe the model.
    """
    deps_win = repo_root / "scripts/build-deps-windows.sh"
    deps_unix = repo_root / "scripts/build-deps-unix.sh"
    publish = repo_root / "scripts/publish-deps-cache.sh"
    build_local_ps1 = repo_root / "scripts/build-local.ps1"
    build_local_sh = repo_root / "scripts/build-local.sh"

    for path, label in [(deps_win, "build-deps-windows.sh"),
                        (deps_unix, "build-deps-unix.sh"),
                        (publish, "publish-deps-cache.sh"),
                        (build_local_sh, "build-local.sh")]:
        if not path.exists():
            return fail(f"Missing deps-cache contract script: {label} ({path})")

    deps_win_text = read_text(deps_win)
    deps_unix_text = read_text(deps_unix)
    for label, text in [("build-deps-windows.sh", deps_win_text),
                        ("build-deps-unix.sh", deps_unix_text)]:
        if "compute_stamp" not in text:
            return fail(f"{label} is missing the stamp-gate function 'compute_stamp'")
        if ".build-stamp" not in text:
            return fail(f"{label} is missing the stamp file path '.build-stamp'")
    if "v0.0.7" not in deps_win_text:
        return fail("build-deps-windows.sh is missing the pinned LIBUVC_REF v0.0.7")

    ps1_text = read_text(build_local_ps1)
    if "Invoke-DepsBuild" not in ps1_text:
        return fail("build-local.ps1 must auto-invoke the deps script via Invoke-DepsBuild (not bail at the deps gate)")
    if "build-deps-windows.sh" not in ps1_text:
        return fail("build-local.ps1 must reference build-deps-windows.sh")

    sh_text = read_text(build_local_sh)
    if "build-deps-unix.sh" not in sh_text:
        return fail("build-local.sh must reference build-deps-unix.sh")

    workflow_text = read_text(workflow_path)
    if "actions/cache@v6" not in workflow_text:
        return fail("build.yml is missing actions/cache@v6 for deps caching")
    if "deps cache hit" not in workflow_text:
        return fail("build.yml is missing the deps cache-hit guard ('deps cache hit')")
    if "v0.0.7" not in workflow_text:
        return fail("build.yml must pin libuvc to v0.0.7 (parity with build-deps-windows.sh)")

    dev_notes_text = read_text(dev_notes_path)
    required_dev_snippets = [
        "Vendored deps caching",
        "build-deps-windows.sh",
        "actions/cache@v4",
        "publish-deps-cache.sh",
    ]
    for snippet in required_dev_snippets:
        if snippet not in dev_notes_text:
            return fail(f"Dev notes are missing deps-cache contract snippet: {snippet}")

    installation_text = read_text(tools_readme_path)
    required_installation_snippets = [
        "Vendored deps caching model",
        "build-deps-windows.sh",
        "build-deps-unix.sh",
        "publish-deps-cache.sh",
    ]
    for snippet in required_installation_snippets:
        if snippet not in installation_text:
            return fail(f"INSTALLATION.md is missing deps-cache contract snippet: {snippet}")
    return 0


def func_body_from(source: str, signature: str) -> str:
    """Return the text of the function whose definition starts at
    `signature`, from the signature up to the next column-0 closing brace.
    All target functions keep nested braces indented, so this is exact."""
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"Function signature not found: {signature!r}")
    end = source.find("\n}", start)
    if end < 0:
        raise RuntimeError(f"Function end not found for: {signature!r}")
    return source[start:end]


def check_raw_direct_passthrough_contract(repo_root: Path) -> int:
    """Static contract for the direct native RAW passthrough path (CXADC,
    FLAC off, no resampling): tap placement before the A/B pairing
    truncation, shared eligibility predicate, naming by content
    (.u8/.u16 direct, .s8/.s16 converted), single producer per record
    ring, and the start/stop tap handshakes."""
    record_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c")
    record_h = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.h")
    direct_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record_direct.c")
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")
    cxadc_h = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.h")
    extract_c = read_text(repo_root / "misrc_tools/misrc_gui/processing/gui_extract.c")
    extract_h = read_text(repo_root / "misrc_tools/misrc_gui/processing/gui_extract.h")
    settings_c = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings_table.c")
    meson = read_text(repo_root / "misrc_tools/meson.build")
    harness_c = read_text(repo_root / "misrc_tools/test/gui_record_direct_harness.c")

    required_snippets = [
        (direct_c, "atomic_fetch_add(&ctx->tap_inflight, 1);", "producer marks in-flight"),
        (direct_c, "atomic_load(&ctx->tap_enabled)", "producer checks the enabled flag"),
        (direct_c, "bufmgr_fill_level(ctx->bufmgr, (buffer_id_t)ctx->buf_id)", "writer reads variable lengths"),
        (direct_c, "GUI_RECORD_DIRECT_MAX_BLOCK_BYTES", "writer read granularity cap"),
        (cxadc_c, "gui_record_direct_tap_push_latched(0, true, card_buf_a", "card A latched native tap"),
        (cxadc_c, "gui_record_direct_tap_push_latched(1, true, card_buf_b", "card B latched native tap"),
        (cxadc_c, "gui_record_tap_iteration_enter(0)", "iteration gate enter (A)"),
        (cxadc_c, "gui_record_tap_iteration_enter(1)", "iteration gate enter (B)"),
        (cxadc_c, "gui_record_tap_iteration_leave(0)", "iteration gate leave (A)"),
        (cxadc_c, "gui_record_tap_iteration_leave(1)", "iteration gate leave (B)"),
        (cxadc_c, "bool tap_this_iter = a_on && b_on;", "per-iteration latched record decision (A/B chunk symmetry)"),
        (record_h, "gui_record_direct_tap_push_latched", "latched tap wrapper declaration"),
        (direct_c, "gui_record_direct_push_latched(", "latched push in the record-direct module"),
        (cxadc_h, "gui_cxadc_direct_record_available", "CXADC direct availability query"),
        (record_h, "gui_record_direct_tap_push", "tap entry point declaration"),
        (record_h, "gui_record_spill_read_block", "public spill read"),
        (record_h, "gui_record_spill_backlog_bytes", "public spill backlog query"),
        (record_c, '"Recording (RAW direct)..."', "direct status text"),
        (record_c, "RAW direct native passthrough", "capture_format log line"),
        (record_c, "gui_record_direct_channel_eligible", "shared eligibility predicate"),
        (record_c, "Direct RAW channel A: bytes_read=", "end summary read vs written"),
        (extract_h, "gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b,", "set_recording carries the direct mask"),
        (meson, "'misrc_gui/output/gui_record_direct.c'", "module in the GUI build"),
        (meson, "test('gui_record_direct'", "harness registered as a meson test"),
        (harness_c, "gui_record_direct_push(", "harness drives the real push"),
        (harness_c, "gui_record_direct_writer_thread", "harness drives the real writer"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"Direct RAW passthrough contract missing {label}: {snippet}")

    # Tap placement: the lockstep tap gate must sit AFTER the A/B reads and
    # pairing truncation (the pushed bytes are the exact card reads, never
    # the paired/truncated samples) and BEFORE the display-ring write, with
    # the per-iteration ordering: enter both channels' taps, latch the
    # decision once, push both chunks with the same decision, then leave.
    # A record start/stop handshake landing between the per-channel pushes
    # then cannot split the channels by one chunk.
    gate_enter_a = cxadc_c.find("gui_record_tap_iteration_enter(0)")
    gate_latch = cxadc_c.find("bool tap_this_iter = a_on && b_on;")
    tap_a = cxadc_c.find("gui_record_direct_tap_push_latched(0, true, card_buf_a")
    tap_b = cxadc_c.find("gui_record_direct_tap_push_latched(1, true, card_buf_b")
    gate_leave_a = cxadc_c.find("gui_record_tap_iteration_leave(0)")
    truncation = cxadc_c.find("output_samples_b < output_samples")
    display_write = cxadc_c.find("bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF")
    if min(gate_enter_a, gate_latch, tap_a, tap_b, gate_leave_a, truncation, display_write) < 0:
        return fail("Direct RAW tap gate placement anchors not found in gui_cxadc.c")
    if not (truncation < gate_enter_a < gate_latch < tap_a < tap_b < gate_leave_a):
        return fail("Direct RAW tap gate must sit after the A/B pairing, latching the decision once before both latched pushes")
    if not (gate_leave_a < display_write):
        return fail("Direct RAW tap gate must complete before the display-ring write")

    # Producer handshake: mark in-flight, THEN read enabled.
    push_body = func_body_from(direct_c, "gui_record_direct_push_result_t gui_record_direct_push")
    if push_body.find("atomic_fetch_add(&ctx->tap_inflight, 1);") > push_body.find("atomic_load(&ctx->tap_enabled)"):
        return fail("Direct RAW push must mark in-flight before reading tap_enabled")

    # Producer ordering rule: while the spill backlog is non-zero, pushes go
    # to the spill (never into the ring ahead of older spill data).
    if "backlog > 0" not in push_body:
        return fail("Direct RAW push must key its spill-vs-ring decision on the spill backlog")

    # Writer drain order: ringbuffer before spill (oldest data first).
    writer_body = func_body_from(direct_c, "int gui_record_direct_writer_thread(void *ctx_ptr)")
    if writer_body.find("bufmgr_read_begin") > writer_body.find("ctx->cb.spill_read"):
        return fail("Direct RAW writer must drain the ringbuffer before the spill backlog")

    # Naming by content: direct -> .u8/.u16, converted -> .s8/.s16.
    ext_body = func_body_from(record_c, "static const char *raw_ext_for_bits")
    for snippet in ['"u8"', '"u16"', '"s8"', '"s16"']:
        if snippet not in ext_body:
            return fail(f"gui_record.c raw_ext_for_bits must map direct/converted to u8/u16/s8/s16: missing {snippet}")
    settings_ext_body = func_body_from(settings_c, "static const char *raw_ext_for_bits")
    for snippet in ['"s8"', '"s16"']:
        if snippet not in settings_ext_body:
            return fail(f"gui_settings_table.c raw_ext_for_bits (converted preview) missing {snippet}")
    for snippet in ['"u8"', '"u16"']:
        if snippet in settings_ext_body:
            return fail(f"gui_settings_table.c load-time preview must not claim native naming: {snippet}")

    # Predicate gates on: CXADC device, FLAC off, per-channel resample off,
    # running capture, live card sample-width match.
    pred_body = func_body_from(record_c, "static bool gui_record_direct_channel_eligible")
    for snippet in ["DEVICE_TYPE_CXADC", "app->settings.use_flac", "enable_resample_a", "enable_resample_b",
                    "gui_cxadc_is_running", "gui_cxadc_direct_record_available"]:
        if snippet not in pred_body:
            return fail(f"Direct RAW eligibility predicate missing required condition: {snippet}")

    # Single producer per record ring: extraction skips direct channels.
    extract_body = func_body_from(extract_c, "static int extraction_thread(void *ctx)")
    for snippet in ["!atomic_load(&s_direct_channel_a)", "!atomic_load(&s_direct_channel_b)"]:
        if snippet not in extract_body:
            return fail(f"Extraction must skip record writes for direct channels: {snippet}")
    set_rec_body = func_body_from(extract_c, "void gui_extract_set_recording")
    for snippet in ["s_direct_channel_a", "s_direct_channel_b"]:
        if snippet not in set_rec_body:
            return fail(f"gui_extract_set_recording must carry the direct mask: {snippet}")

    # Stop handshake: taps disabled and idle BEFORE is_recording clears.
    stop_body = func_body_from(record_c, "void gui_record_stop(gui_app_t *app)")
    tap_disable_pos = stop_body.find("gui_record_direct_tap_disable")
    tap_idle_pos = stop_body.find("gui_record_direct_tap_wait_idle")
    is_recording_pos = stop_body.find("app->is_recording = false;")
    if min(tap_disable_pos, tap_idle_pos, is_recording_pos) < 0:
        return fail("Direct RAW stop handshake anchors not found in gui_record_stop")
    if not (tap_disable_pos < tap_idle_pos < is_recording_pos):
        return fail("gui_record_stop must disable+idle the taps before clearing is_recording")

    # Start order: writers, then taps, then extraction recording (skip mask).
    start_writer = record_c.find("gui_record_direct_writer_thread")
    start_tap = record_c.find("gui_record_direct_tap_enable(&s_direct_ctx_a)")
    start_extract = record_c.find("gui_extract_set_recording(true, false, bits_a, bits_b, direct_a, direct_b)")
    if min(start_writer, start_tap, start_extract) < 0:
        return fail("Direct RAW start-order anchors not found in gui_record.c")
    if not (start_writer < start_tap < start_extract):
        return fail("Record start must start writers, then enable taps, then extraction recording")
    return 0


def check_gui_auto_test_flags(repo_root: Path) -> int:
    """Static contract for the unattended local test flags
    (--select-device / --auto-capture / --auto-record) and the
    overwrite_files popup bypass that unattended runs depend on."""
    gui_c = read_text(repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c")
    record_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_record.c")

    required_snippets = [
        (gui_c, 'strcmp(a, "--select-device")', "--select-device parsed as a GUI flag"),
        (gui_c, 'strcmp(a, "--auto-capture")', "--auto-capture parsed as a GUI flag"),
        (gui_c, 'strcmp(a, "--auto-record")', "--auto-record parsed as a GUI flag"),
        (gui_c, "[AUTO] --select-device: no device matches", "--select-device fails fast on no match"),
        (gui_c, "gui_strcasestr_local", "portable case-insensitive device name match"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"GUI auto-test flags contract missing {label}: {snippet}")

    # The unattended record cycle must report each phase and exit nonzero on failure.
    for marker in ["[AUTO] auto-capture armed", "[AUTO] recording started", "[AUTO] recording stopped",
                   "[AUTO] record cycle complete", "return auto_rec_failed ? 1 : 0;"]:
        if marker not in gui_c:
            return fail(f"GUI auto-record cycle missing required marker: {marker}")

    # usage text documents all three flags
    for flag in ["--select-device", "--auto-capture", "--auto-record <seconds>"]:
        if flag not in gui_c:
            return fail(f"GUI usage text missing auto-test flag: {flag}")

    # overwrite_files must bypass the overwrite confirmation popup so
    # unattended runs cannot stall on it.
    start_body = func_body_from(record_c, "int gui_record_start(gui_app_t *app)")
    if "!app->settings.overwrite_files" not in start_body:
        return fail("gui_record_start must skip the overwrite popup when overwrite_files is on")
    return 0


def check_cxadc_rate_probe_contract(repo_root: Path) -> int:
    """Static contract for the CXADC measured feed-rate check: probe at
    startup enumeration and capture start, tier snapping via the shared
    dependency-free header, effective rate preferred by the UI/settings
    rate getters, and the env opt-out."""
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")
    cxadc_h = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.h")
    capture_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_capture.c")
    ui_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c")
    tiers_h = read_text(repo_root / "misrc_tools/common/cxadc_rate_tiers.h")
    tiers_test_c = read_text(repo_root / "misrc_tools/test/cxadc_rate_tiers_test.c")
    meson = read_text(repo_root / "misrc_tools/meson.build")

    required_snippets = [
        (cxadc_c, "cxadc_measure_card_rate_hz", "timed blocking-read measurement"),
        (cxadc_c, "cxadc_probe_and_cache_card_rate", "probe + cache + sysfs comparison"),
        (cxadc_c, "MISRC_GUI_NO_CXADC_RATE_PROBE", "probe env opt-out"),
        (cxadc_c, "CXADC_RATE_PROBE_START_BYTES", "capture-start probe window"),
        (cxadc_c, "gui_cxadc_get_effective_rate_hz", "effective-rate API"),
        (cxadc_c, "cxadc_snap_rate_tier_hz", "tier snapping from the shared header"),
        (cxadc_c, "s_cxadc_measured_rate_state", "measured-rate cache"),
        (cxadc_c, "CXADC_RATE_PROBE_DRAIN_CHUNK_MIN_US", "backlog drain phase before the timed window"),
        (cxadc_h, "gui_cxadc_get_effective_rate_hz", "effective-rate declaration"),
        (cxadc_h, "gui_cxadc_probe_card_rates", "probe entry point declaration"),
        (capture_c, "gui_cxadc_probe_card_rates(cxadc_card_count", "startup enumeration probe"),
        (capture_c, "gui_cxadc_get_effective_rate_hz(0, tenbit_mode_a", "CXADC profile sync uses the effective rate"),
        (ui_c, "gui_cxadc_get_effective_rate_hz(card_idx, false", "UI base rate uses the effective rate"),
        (ui_c, "gui_cxadc_get_effective_rate_hz(card_idx, tenbit", "UI hw rate uses the effective rate"),
        (tiers_h, "cxadc_snap_rate_tier_hz", "tier snap helper"),
        (tiers_h, "35795454", "upsampled 35.8 exclusion documented/absent as tier"),
        (tiers_test_c, "cxadc_snap_rate_tier_hz", "tier snap unit test drives the real helper"),
        (meson, "test('cxadc_rate_tiers'", "tier snap test registered"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"CXADC rate probe contract missing {label}: {snippet}")

    # The tier table must NOT contain the upsampled 35.8 feed (35795454 /
    # 35800000 must appear only in comments, never as a tier value).
    tiers_body = tiers_h[tiers_h.find("CXADC_RATE_TIER_HZ[] = {"):tiers_h.find("};")]
    for forbidden in ("35795454U,", "35800000U,"):
        if forbidden in tiers_body:
            return fail(f"35.8 MHz upsampled feed must not be a snap tier: {forbidden}")

    # The measured rate must override the sysfs belief when they disagree
    # (the reported stale-crystal case), at every capture start.
    start_body = func_body_from(cxadc_c, "int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)")
    probe_pos = start_body.find("cxadc_probe_and_cache_card_rate")
    override_pos = start_body.find("s_cxadc.card_sample_rate_hz[i] = s_cxadc_measured_rate_hz[i][t]")
    # rfind: the probe block's re-assignment is the LAST occurrence (the
    # original sysfs-derived assignment earlier in the function stays).
    rf_pos = start_body.rfind("s_cxadc.rf_sample_rate_hz = s_cxadc.card_sample_rate_hz[0]")
    if min(probe_pos, override_pos, rf_pos) < 0:
        return fail("Capture-start rate-probe anchors not found in gui_cxadc_start")
    if not (probe_pos < override_pos < rf_pos):
        return fail("gui_cxadc_start must probe, apply the measured rate, then set rf_sample_rate_hz")
    return 0


def check_raw_direct_writer_runtime(repo_root: Path) -> int:
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: direct RAW writer runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for direct RAW writer runtime guard")
        print("SKIP: direct RAW writer runtime guard (cc not available)")
        return 0

    sources = [
        repo_root / "misrc_tools/test/gui_record_direct_harness.c",
        repo_root / "misrc_tools/misrc_gui/output/gui_record_direct.c",
        repo_root / "misrc_tools/common/buffer_manager.c",
        repo_root / "misrc_tools/common/ringbuffer.c",
        repo_root / "misrc_tools/common/rb_event.c",
    ]
    for path in sources:
        if not path.exists():
            return fail(f"Direct RAW writer runtime source is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="misrc_direct_raw_guard_") as temp_root:
        exe_name = "gui_record_direct_guard.exe" if os.name == "nt" else "gui_record_direct_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        compile_cmd += [str(p) for p in sources]
        if sys.platform.startswith("linux"):
            compile_cmd += ["-lpthread"]
        compile_cmd += ["-o", str(exe_path)]
        try:
            run_checked(compile_cmd)
        except subprocess.CalledProcessError as exc:
            return fail(
                "Failed to compile direct RAW writer runtime harness\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
        try:
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "Direct RAW writer runtime harness failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_record_ringbuffer_fallback_runtime(repo_root: Path) -> int:
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: record ringbuffer fallback runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for record ringbuffer fallback runtime guard")
        print("SKIP: record ringbuffer fallback runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/bufmgr_record_fallback_harness.c"
    buffer_manager_path = repo_root / "misrc_tools/common/buffer_manager.c"
    include_dir = repo_root / "misrc_tools/common"

    if not harness_path.exists():
        return fail(f"Record fallback harness source is missing: {harness_path}")
    if not buffer_manager_path.exists():
        return fail(f"buffer_manager.c is missing: {buffer_manager_path}")

    with tempfile.TemporaryDirectory(prefix="misrc_bufmgr_guard_") as temp_root:
        exe_name = "bufmgr_record_fallback_guard.exe" if os.name == "nt" else "bufmgr_record_fallback_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            str(buffer_manager_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        try:
            run_checked(compile_cmd)
        except subprocess.CalledProcessError as exc:
            return fail(
                "Failed to compile record ringbuffer fallback runtime harness\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )

        try:
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "Record ringbuffer fallback runtime harness failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_net_fanout_runtime(repo_root: Path) -> int:
    """The /rf and /baseband streams are fed by the net fanout
    (misrc_gui/net/gui_net_fanout.c). The original pinned the queue head
    forever (every wake-up replayed the stream from chunk #1), never released
    the producer's reference (unbounded growth at RF rate for as long as a
    client was connected), attached to queued data only after a successful wait
    (a reader with data already waiting slept until the next push), and
    reported shutdown as a 0-byte timeout (the handler loop never exited, so
    server_stop() freed the queue under it). Compiles the real module against a
    harness that drives it the way the handler does and measures delivered vs
    pushed vs dropped, ordering, wake-up latency, memory held after a drain and
    the shutdown hand-off."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: net fanout runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the net fanout runtime guard")
        print("SKIP: net fanout runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/net_fanout_harness.c"
    fanout_path = repo_root / "misrc_tools/misrc_gui/net/gui_net_fanout.c"
    net_include = repo_root / "misrc_tools/misrc_gui/net"

    for required in (harness_path, fanout_path):
        if not required.exists():
            return fail(f"Net fanout guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_net_fanout_guard_") as temp_root:
        exe_path = Path(temp_root) / "net_fanout_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            "-pthread",
            f"-I{net_include}",
            str(harness_path),
            str(fanout_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Net fanout harness failed to compile:\n{built.stderr.strip()}")
        try:
            ran = subprocess.run([str(exe_path)], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired as exc:
            return fail(f"Net fanout harness hung for {exc.timeout}s (a reader never woke up)")
        if ran.returncode != 0:
            return fail(
                "Net fanout harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_bufmgr_write_tap_runtime(repo_root: Path) -> int:
    """The network server's /rf and /baseband streams are fed by a producer-side
    tap on the buffer manager (bufmgr_set_write_tap): bufmgr_write_end() hands
    the committed region to the tap on the writer's thread, so every capture
    backend (hsdaoh, CXADC, DdD, FX3, RTL-SDR, playback, simulated) streams
    without calling into the net module. Before the tap existed only the hsdaoh
    callback fed the fanout and a CXADC server streamed nothing. Compiles
    buffer_manager.c against a harness with flat ringbuffer stubs and checks
    the tap sees the right region, after commit, and only when installed."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: buffer manager write tap runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the buffer manager write tap runtime guard")
        print("SKIP: buffer manager write tap runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/bufmgr_write_tap_harness.c"
    buffer_manager_path = repo_root / "misrc_tools/common/buffer_manager.c"
    include_dir = repo_root / "misrc_tools/common"

    for required in (harness_path, buffer_manager_path):
        if not required.exists():
            return fail(f"Buffer manager write tap guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_bufmgr_tap_guard_") as temp_root:
        exe_path = Path(temp_root) / "bufmgr_write_tap_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            str(buffer_manager_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Buffer manager write tap harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Buffer manager write tap harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_ringbuffer_mirror_runtime(repo_root: Path) -> int:
    """Every capture, record and net-ingest path sits on common/ringbuffer.c's
    mirrored mapping, and --smoke-test returns before it is ever created, so a
    host where rb_init() fails looks healthy in CI and captures nothing in use.
    macOS 26 rejects O_NOFOLLOW on shm_open() with EINVAL, which shm_anon.h's
    POSIX path passed unconditionally: every ring failed there with rc 2.
    Compiles the real ringbuffer.c against a harness that creates rings and
    proves the two halves alias."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: ringbuffer mirror runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the ringbuffer mirror runtime guard")
        print("SKIP: ringbuffer mirror runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/ringbuffer_mirror_harness.c"
    ringbuffer_path = repo_root / "misrc_tools/common/ringbuffer.c"
    include_dir = repo_root / "misrc_tools/common"

    for required in (harness_path, ringbuffer_path):
        if not required.exists():
            return fail(f"Ringbuffer mirror guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_rb_mirror_guard_") as temp_root:
        exe_path = Path(temp_root) / "ringbuffer_mirror_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_POSIX_C_SOURCE=200809L",
            "-D_DEFAULT_SOURCE",
            "-D_GNU_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            str(ringbuffer_path),
            "-o",
            str(exe_path),
        ]
        if sys.platform == "darwin":
            compile_cmd.insert(3, "-D_DARWIN_C_SOURCE")
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Ringbuffer mirror harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Ringbuffer mirror harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_preview_sdtv_geometry_runtime(repo_root: Path) -> int:
    """The SDTV preview geometry arithmetic, proved on every build.

    This is the only proof the 625-line answers ever get: there is one NTSC
    dongle on this site and no PAL or SECAM source at all, so PAL's 720x576
    raster and its 16:15 sample aspect cannot be checked against hardware here.
    """
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the SDTV preview geometry guard")
        print("SKIP: SDTV preview geometry guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/preview_sdtv_harness.c"
    policy_path = repo_root / "misrc_tools/misrc_gui/input/gui_preview_sdtv.c"
    include_dir = repo_root / "misrc_tools/misrc_gui/input"

    for path, label in [(harness_path, "SDTV preview harness"),
                        (policy_path, "SDTV preview geometry")]:
        if not path.exists():
            return fail(f"{label} source is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="misrc_preview_sdtv_guard_") as temp_root:
        exe_name = "preview_sdtv_guard.exe" if os.name == "nt" else "preview_sdtv_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            f"-I{include_dir}",
            str(harness_path),
            str(policy_path),
            "-lm",
            "-o",
            str(exe_path),
        ]
        try:
            run_checked(compile_cmd)
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "SDTV preview geometry guard failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_usbref_ui_stays_out_of_gui_ui(repo_root: Path) -> int:
    """The USB reference video UI must stay in its own file.

    ui/gui_ui.c is upstream's. Roughly 930 lines of this fork's reference-video
    and streaming UI used to live in it, and every one was a conflict on every
    upstream sync. Moving them out only helps for as long as they stay out, and
    a new row is always easier to add where the other rows already are -- which
    is exactly how it accumulated the first time.

    gui_ui.c keeps the handful of wiring lines that raise the dialog. Anything
    that draws or handles one of its controls belongs in
    ui/gui_usbref_settings.c."""
    ui = strip_c_comments(read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"))

    # Clay ids this fork's dialog owns. The wiring lines name the window itself,
    # never a control inside it.
    banned = ("RtspCodec", "RtspLanConfirm", "RtspBindBox", "RtspEncoderBox",
              "RtspUrl", "RtspPassword", "RtspLiveBox", "ToggleRtsp",
              "PreviewDeviceBox", "PreviewRescanBtn", "PreviewConnectBtn",
              "PreviewAspectBox", "PreviewCrop", "ToggleCcRecord", "CcTagField",
              "VideoCodecBox")
    found = [b for b in banned if b in ui]
    if found:
        return fail(
            "gui_ui.c draws or handles USB reference video controls again: "
            + ", ".join(found) + ".\n"
            "That file is upstream's. These belong in ui/gui_usbref_settings.c; "
            "gui_ui.c keeps only the gear that opens the dialog and its five "
            "wiring lines."
        )

    # Reading a usbref_* setting here is NOT banned, deliberately. Three
    # legitimate readers remain and would be worse anywhere else:
    #   - the text-field buffer map, because the edit machinery is gui_ui.c's;
    #   - gui_ui_apply_remote_setting(), the server side of the net setter,
    #     which gates by key string and must keep refusing stream changes
    #     mid-stream;
    #   - the Capture hint line, which names the codec in force for the
    #     Reference video row that stays there.
    # What must not come back is a CONTROL, which the ids above catch.
    return 0


def check_preview_crop_never_reaches_a_recording(repo_root: Path) -> int:
    """The preview crop is a viewing aid and must stay one.

    The reference MKV exists to be compared frame-for-frame against a
    tbc-video-export of the same tape, so it has to keep the full active
    raster. A crop filter in either encoder argv builder would silently break
    that comparison, and it would look like a feature while doing it.
    """
    for rel in ("misrc_tools/misrc_gui/output/gui_video_record.c",
                "misrc_tools/misrc_gui/streaming/gui_rtsp_stream.c"):
        path = repo_root / rel
        if not path.exists():
            return fail(f"encoder source is missing: {path}")
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
        for needle in ("crop=", "\"crop\"", "drawbox"):
            if needle in text:
                return fail(
                    f"{rel} builds an ffmpeg crop/mask filter ({needle}).\n"
                    "The preview crop is preview-only by design: the reference "
                    "recording keeps the full active raster so it stays "
                    "frame-comparable with a tbc-video-export. Crop at the "
                    "render path in gui_preview_v4l2.c instead."
                )
    return 0


def check_ui_scale_policy_runtime(repo_root: Path) -> int:
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for UI scale policy runtime guard")
        print("SKIP: UI scale policy runtime guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/gui_ui_scale_harness.c"
    policy_path = repo_root / "misrc_tools/misrc_gui/ui/gui_ui_scale.c"
    include_dir = repo_root / "misrc_tools/misrc_gui/ui"

    for path, label in [(harness_path, "UI scale harness"),
                        (policy_path, "UI scale policy")]:
        if not path.exists():
            return fail(f"{label} source is missing: {path}")

    with tempfile.TemporaryDirectory(prefix="misrc_ui_scale_guard_") as temp_root:
        exe_name = "gui_ui_scale_guard.exe" if os.name == "nt" else "gui_ui_scale_guard"
        exe_path = Path(temp_root) / exe_name
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            f"-I{include_dir}",
            str(harness_path),
            str(policy_path),
            "-lm",
            "-o",
            str(exe_path),
        ]
        try:
            run_checked(compile_cmd)
            run_checked([str(exe_path)])
        except subprocess.CalledProcessError as exc:
            return fail(
                "UI scale policy runtime guard failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_flac_streaminfo_total_samples_runtime(repo_root: Path) -> int:
    """Encode a real FLAC via flac_writer and assert the STREAMINFO
    total_samples contract: exact count below 2^36, 0 (unknown) above.
    Guards against reintroducing the /1000 "duration scaling" that made
    libsndfile readers (hifi-decode) silently truncate decodes."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        print("SKIP: FLAC STREAMINFO runtime guard (Linux/macOS only)")
        return 0
    cc = shutil.which("cc")
    pkg_config = shutil.which("pkg-config")
    if cc is None or pkg_config is None:
        print("SKIP: FLAC STREAMINFO runtime guard (cc/pkg-config not available)")
        return 0
    flac_flags = subprocess.run(
        [pkg_config, "--cflags", "--libs", "flac"], capture_output=True, text=True
    )
    if flac_flags.returncode != 0:
        print("SKIP: FLAC STREAMINFO runtime guard (libFLAC not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/flac_writer_streaminfo_test.c"
    flac_writer_path = repo_root / "misrc_tools/common/flac_writer.c"
    include_dir = repo_root / "misrc_tools/common"
    if not harness_path.exists():
        return fail(f"FLAC STREAMINFO harness source is missing: {harness_path}")
    if not flac_writer_path.exists():
        return fail(f"flac_writer.c is missing: {flac_writer_path}")

    with tempfile.TemporaryDirectory(prefix="misrc_flac_streaminfo_guard_") as temp_root:
        exe_path = Path(temp_root) / "flac_writer_streaminfo_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-DLIBFLAC_ENABLED=1",
            f"-I{include_dir}",
            str(harness_path),
            str(flac_writer_path),
        ] + flac_flags.stdout.split() + [
            "-lpthread",
            "-o",
            str(exe_path),
        ]
        try:
            run_checked(compile_cmd)
        except subprocess.CalledProcessError as exc:
            return fail(
                "Failed to compile FLAC STREAMINFO runtime harness\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
        try:
            run_checked([str(exe_path), str(Path(temp_root) / "guard.flac")])
        except subprocess.CalledProcessError as exc:
            return fail(
                "FLAC STREAMINFO runtime harness failed\n"
                f"stdout:\n{exc.stdout}\n"
                f"stderr:\n{exc.stderr}"
            )
    return 0


def check_priority_clamp_runtime(repo_root: Path) -> int:
    """threading.h's realtime and nice requests must degrade to what
    RLIMIT_RTPRIO / RLIMIT_NICE allow instead of failing outright. On hosts
    capped at LimitRTPRIO=20 / LimitNICE=-11 the all-or-nothing request left
    every capture thread SCHED_OTHER -- "[THREAD] Thread priority request 3
    could not be elevated (rt_err=1, nice_err=13)" in the journal. Also covers
    the per-thread I/O priority helper: set, restore, inherited by a spawned
    child.

    The clamp cases need a non-root user with a nonzero hard RLIMIT_RTPRIO /
    RLIMIT_NICE. A runner service on systemd's defaults (both 0) or a root
    container has neither, so those cases report SKIP and only the I/O case
    runs -- the skips are printed so a green run never hides them."""
    if not sys.platform.startswith("linux"):
        print("SKIP: priority clamp guard (Linux only)")
        return 0
    cc = shutil.which("cc")
    if cc is None:
        if os.environ.get("GITHUB_ACTIONS", "").lower() == "true":
            return fail("C compiler 'cc' is required for the priority clamp guard")
        print("SKIP: priority clamp guard (cc not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/priority_clamp_harness.c"
    include_dir = repo_root / "misrc_tools/common"
    if not harness_path.exists():
        return fail(f"Priority clamp harness source is missing: {harness_path}")

    with tempfile.TemporaryDirectory(prefix="misrc_priority_clamp_guard_") as temp_root:
        exe_path = Path(temp_root) / "priority_clamp_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            # threading.h's thrd_create casts int(*)(void*) to pthread's
            # void*(*)(void*); -Wextra flags that, and it is not this guard's concern.
            "-Wno-cast-function-type",
            "-D_GNU_SOURCE",
            f"-I{include_dir}",
            str(harness_path),
            "-lpthread",
            "-o",
            str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"Priority clamp harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "Priority clamp harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
        for line in ran.stdout.splitlines():
            if line.startswith("SKIP:"):
                print(f"SKIP: priority clamp case -{line[len('SKIP:'):]}")
    return 0


def check_flac_worker_priority_runtime(repo_root: Path) -> int:
    """libFLAC starts its encoder workers lazily from inside
    FLAC__stream_encoder_process(), on the calling thread, and a new thread
    copies its creator's policy -- so the SCHED_FIFO RF writer would hand up to
    eight CPU-bound workers per channel SCHED_FIFO, and with RT throttling off a
    lagging encode starves the host. flac_writer must run every worker in the
    strongest normal class and put the writer back on SCHED_FIFO afterwards.

    Needs a writer that may become SCHED_FIFO (nonzero RLIMIT_RTPRIO, or root)
    and libFLAC built with threads; otherwise it reports SKIP."""
    if not sys.platform.startswith("linux"):
        print("SKIP: FLAC worker priority guard (Linux only)")
        return 0
    cc = shutil.which("cc")
    pkg_config = shutil.which("pkg-config")
    if cc is None or pkg_config is None:
        print("SKIP: FLAC worker priority guard (cc/pkg-config not available)")
        return 0
    flac_flags = subprocess.run(
        [pkg_config, "--cflags", "--libs", "flac"], capture_output=True, text=True
    )
    if flac_flags.returncode != 0:
        print("SKIP: FLAC worker priority guard (libFLAC not available)")
        return 0

    harness_path = repo_root / "misrc_tools/test/flac_worker_priority_harness.c"
    flac_writer_path = repo_root / "misrc_tools/common/flac_writer.c"
    include_dir = repo_root / "misrc_tools/common"
    for required in (harness_path, flac_writer_path):
        if not required.exists():
            return fail(f"FLAC worker priority guard source is missing: {required}")

    with tempfile.TemporaryDirectory(prefix="misrc_flac_worker_priority_guard_") as temp_root:
        exe_path = Path(temp_root) / "flac_worker_priority_guard"
        compile_cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-D_GNU_SOURCE",
            "-DLIBFLAC_ENABLED=1",
            "-DHAVE_FLAC_THREADING=1",
            f"-I{include_dir}",
            str(harness_path),
            str(flac_writer_path),
        ] + flac_flags.stdout.split() + [
            "-lpthread",
            "-o",
            str(exe_path),
        ]
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"FLAC worker priority harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode == 2:
            print(f"SKIP: FLAC worker priority guard -{ran.stdout.strip()[len('SKIP:'):]}")
            return 0
        if ran.returncode != 0:
            return fail(
                "FLAC worker priority harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
            )
    return 0


def check_ui_scale_integration_contract(repo_root: Path, gui_c_path: Path,
                                        gui_settings_c_path: Path,
                                        meson_path: Path) -> int:
    gui_c = read_text(gui_c_path)
    settings_c = read_text(gui_settings_c_path)
    # The struct and the table moved out of gui_app.h / gui_settings.c so the
    # settings code compiles without raylib; the contract follows them.
    settings_table_c = read_text(gui_settings_c_path.with_name("gui_settings_table.c"))
    gui_settings_h = read_text(gui_settings_c_path.with_name("gui_settings.h"))
    gui_ui_h = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.h")
    gui_ui_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c")
    popup_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/gui_popup.c")
    renderer_c = read_text(repo_root / "misrc_tools/misrc_gui/ui/clay_renderer_raylib.c")
    oscilloscope_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_oscilloscope.c")
    fft_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_fft.c")
    phosphor_h = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_phosphor_rt.h")
    phosphor_c = read_text(repo_root / "misrc_tools/misrc_gui/visualization/gui_phosphor_rt.c")
    meson = read_text(meson_path)

    required_snippets = [
        (gui_settings_h, "int ui_scale_percent;", "persisted settings field"),
        (settings_table_c, "settings->ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;", "100% default"),
        (settings_table_c, '"ui_scale_percent",', "settings table key"),
        (settings_table_c, "gui_ui_scale_parse_percent(value)", "validated settings load"),
        (settings_c, "GUI_SETTINGS_MAX_FILE_BYTES", "settings file written through the table"),
        (meson, "'misrc_gui/ui/gui_ui_scale.c'", "UI scale policy product source"),
        (gui_c, "gui_ui_zoom_process(&ui_zoom_state", "single wheel routing policy"),
        (gui_c, "IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0)", "100% reset shortcut"),
        (gui_c, "IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)", "keyboard zoom-in shortcut"),
        (gui_c, "IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)", "keyboard zoom-out shortcut"),
        # The keyboard steps the desktop-relative zoom, the same value the wheel does.
        (gui_c, "gui_ui_zoom_step_percent(app.settings.ui_zoom_percent", "shared keyboard zoom-step policy"),
        (gui_c, "ui_zoom_result.step_attempted || keyboard_zoom_pressed", "zoom HUD attempt feedback"),
        (gui_c, "gui_ui_show_scale_hud(ui_zoom_result.percent);", "zoom HUD trigger"),
        (gui_c, "ui_zoom_result.passthrough_x * 20.0f", "Clay horizontal wheel routing"),
        (gui_c, "ui_zoom_result.passthrough_y * 20.0f", "Clay vertical wheel routing"),
        (gui_ui_h, "Vector2 gui_ui_get_mouse_position(void);", "logical pointer API"),
        (gui_ui_c, "position.x /= scale;", "pointer inverse transform"),
        (gui_ui_c, "CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH", "pointer-transparent zoom HUD"),
        (gui_ui_c, "gui_ui_scale_hud_opacity(remaining_s)", "zoom HUD fade policy"),
        (gui_ui_c, "render_ui_scale_hud();", "zoom HUD render integration"),
        (gui_ui_c, "Cmd+0 to reset to 100%", "macOS zoom reset hint"),
        (gui_ui_c, "Ctrl+0 to reset to 100%", "desktop zoom reset hint"),
        (gui_ui_c, "gui_ui_toolbar_uses_two_rows(toolbar_width,", "content-aware toolbar policy"),
        (gui_ui_c, "gui_ui_get_status_layout_mode(status_width, status_height,", "responsive status policy"),
        (gui_ui_c, 'strstr(raw_status_gate, "Capture stopped:")', "critical stop reason priority"),
        (gui_ui_c, "gui_ui_modal_max_extent(gui_ui_get_layout_width()", "viewport-clamped modals"),
        (popup_c, "gui_ui_modal_max_extent(gui_ui_get_layout_width()", "viewport-clamped generic popup"),
        (popup_c, 'CLAY_ID("PopupContentScroll")', "scrollable popup content"),
        (popup_c, "CLAY_SIZING_FIT(.max = popup_message_max_width)", "viewport-bounded popup text"),
        (renderer_c, "Matrix outer_modelview = rlGetMatrixModelview();", "outer render transform capture"),
        (renderer_c, "rlScalef(ui_scale, ui_scale, 1.0f);", "global render transform"),
        (renderer_c, "box.x * ui_scale", "scaled scissor transform"),
        (renderer_c, "rlSetMatrixModelview(outer_modelview);", "balanced render transform"),
        (phosphor_h, "Matrix outer_modelview;", "saved phosphor model-view"),
        (phosphor_c, "rlGetMatrixModelview()", "phosphor transform capture"),
        (phosphor_c, "rlSetMatrixModelview", "phosphor transform restore"),
        (phosphor_c, "DrawTexturePro", "logical-size phosphor composite"),
        (gui_ui_h, "Vector2 gui_ui_get_render_scale(void);", "framebuffer-density API"),
        (oscilloscope_c, "gui_ui_get_render_scale();", "scale-aware waveform texture"),
        (fft_c, "gui_ui_get_render_scale();", "scale-aware FFT texture"),
    ]
    for source, snippet, label in required_snippets:
        if snippet not in source:
            return fail(f"Missing UI scale integration contract ({label}): {snippet}")

    if gui_c.count("GetMouseWheelMoveV(") != 1:
        return fail("misrc_gui.c must snapshot GetMouseWheelMoveV() exactly once per frame")
    if re.search(r"\bGetMouseWheelMove\(", gui_c):
        return fail("misrc_gui.c must not re-read scalar GetMouseWheelMove()")

    ordered = [
        gui_c.find("GetMouseWheelMoveV("),
        gui_c.find("gui_ui_zoom_process(&ui_zoom_state"),
        gui_c.find("Clay_UpdateScrollContainers"),
        gui_c.find("panel_handle_all_scrolls"),
    ]
    if any(pos < 0 for pos in ordered) or ordered != sorted(ordered):
        return fail("UI scale wheel routing must occur before Clay and panel consumers")

    modifier_snippets = [
        "KEY_LEFT_CONTROL", "KEY_RIGHT_CONTROL",
        "KEY_LEFT_SUPER", "KEY_RIGHT_SUPER",
    ]
    for snippet in modifier_snippets:
        if snippet not in gui_c:
            return fail(f"UI scale primary modifier mapping is missing {snippet}")

    direct_mouse_calls = []
    gui_root = repo_root / "misrc_tools/misrc_gui"
    for source_path in gui_root.rglob("*.c"):
        source = read_text(source_path)
        count = source.count("GetMousePosition(")
        if count:
            direct_mouse_calls.append((source_path, count))
    expected_pointer_source = repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"
    if direct_mouse_calls != [(expected_pointer_source, 1)]:
        details = ", ".join(f"{path.relative_to(repo_root)}:{count}"
                            for path, count in direct_mouse_calls)
        return fail(f"Raw GetMousePosition() escaped the logical pointer wrapper: {details}")

    return 0


def check_session_launch_file_contract(repo_root: Path) -> int:
    """--session <path.json> pre-fills one capture from a caller's file. It
    must stay a GUI flag (an unrecognised arg routes the whole process into
    headless CLI capture), its overlay must never reach the settings file
    (gui_settings_save runs the persist filter on a copy), and its "asset"
    links the capture through the capture-metadata store, which lives
    outside gui_settings_t (no ingest_* field may come back)."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        gui_c = strip_c_comments(read_text(base / "core/misrc_gui.c"))
        settings_c = strip_c_comments(read_text(base / "core/gui_settings.c"))
        record_c = strip_c_comments(read_text(base / "output/gui_record.c"))
        meson = read_text(repo_root / "misrc_tools/meson.build")
    except OSError as exc:
        return fail(f"session launch file guard: cannot read a source file: {exc}")
    if "'misrc_gui/core/gui_session.c'" not in meson:
        return fail("meson.build: misrc_gui/core/gui_session.c is not in the GUI sources")
    session_pos = gui_c.find('strcmp(a, "--session") == 0')
    capture_pos = gui_c.find("has_capture_arg = true;")
    if session_pos < 0 or capture_pos < 0 or session_pos > capture_pos:
        return fail("misrc_gui.c: --session must be classified as a GUI flag before the "
                    "capture-arg fallthrough, or it routes into headless CLI capture mode")
    if '"--session-selftest"' not in gui_c or "gui_session_selftest_main()" not in gui_c:
        return fail("misrc_gui.c: --session-selftest no longer runs gui_session_selftest_main()")
    load_pos = gui_c.find("gui_settings_load(&app.settings);")
    apply_pos = gui_c.find("gui_session_apply_file(session_path, &app.settings")
    if load_pos < 0 or apply_pos < 0 or apply_pos < load_pos:
        return fail("misrc_gui.c: the --session overlay must be applied after gui_settings_load()")
    if "[--session <path.json>]" not in gui_c:
        return fail("misrc_gui.c: --help no longer lists [--session <path.json>] "
                    "(callers detect support by grepping --help)")
    try:
        save_body = extract_function_body(settings_c, "void gui_settings_save(const gui_settings_t *settings)")
    except RuntimeError as exc:
        return fail(f"gui_settings.c: {exc}")
    filter_pos = save_body.find("s_persist_filter(filtered)")
    format_pos = save_body.find("gui_settings_format_file(")
    if filter_pos < 0 or format_pos < 0 or filter_pos > format_pos:
        return fail("gui_settings.c: gui_settings_save() must run the persist filter on a copy "
                    "before formatting, or a --session overlay is saved over the operator's settings")
    session_c = strip_c_comments(read_text(base / "core/gui_session.c"))
    apply_pos = session_c.find("bool gui_session_apply(")
    apply_body = session_c[apply_pos:session_c.find("\n}", apply_pos)] if apply_pos >= 0 else ""
    if "gui_capture_meta_set_linked(" not in apply_body:
        return fail("gui_session.c: gui_session_apply() must link the asset through "
                    "gui_capture_meta_set_linked() (capture metadata lives outside the settings)")
    if not all(n in apply_body for n in ('"capture_a"', '"capture_b"', "gui_capture_meta_set_rf_request(")):
        return fail("gui_session.c: gui_session_apply() must overlay rf_channels onto capture_a/capture_b "
                    "(run-only, restored on save) and keep the request through "
                    "gui_capture_meta_set_rf_request()")
    if "Session tag" in record_c:
        return fail("gui_record.c: the retired \"Session tag\" log lines are back; a session's asset "
                    "reaches the log as \"Capture metadata <key>: <value>\"")
    if "gui_capture_meta_init()" not in gui_c or gui_c.find("gui_capture_meta_init()") > gui_c.find(
            "gui_session_apply_file(session_path, &app.settings"):
        return fail("misrc_gui.c: gui_capture_meta_init() must run before the --session file is applied")
    settings_h = strip_c_comments(read_text(base / "core/gui_settings.h"))
    if "ingest_" in settings_h:
        return fail("gui_settings.h: an ingest_* field is back in gui_settings_t. Capture metadata lives "
                    "in core/gui_capture_meta.h and is never saved (SETTINGS_KEYS_RETIRED)")
    return 0


def check_record_gate_post_build(gui_path: Path) -> int:
    """Run the built GUI's --record-gate-selftest: with a finalize in flight
    gui_record_start refuses, and with none it gets past the gate."""
    try:
        ran = subprocess.run([str(gui_path), "--record-gate-selftest"], capture_output=True,
                             text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return fail("misrc_gui --record-gate-selftest timed out")
    if ran.returncode != 0 or "record gate selftest passed" not in ran.stdout:
        return fail(f"misrc_gui --record-gate-selftest failed (rc={ran.returncode}):\n"
                    f"{ran.stdout.strip()}\n{ran.stderr.strip()}")
    print("record gate selftest passed")
    return 0


def check_session_launch_file_post_build(gui_path: Path) -> int:
    """Run the built GUI's --session-selftest (overlay applied, never saved,
    bad files refused) and confirm --help advertises --session."""
    if gui_path.suffix != ".exe":
        helped = subprocess.run([str(gui_path), "--help"], capture_output=True, text=True, timeout=60)
        if "--session <path.json>" not in helped.stdout:
            return fail("misrc_gui --help does not list --session <path.json>")
    try:
        ran = subprocess.run([str(gui_path), "--session-selftest"], capture_output=True,
                             text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return fail("misrc_gui --session-selftest timed out")
    if ran.returncode != 0:
        return fail(f"misrc_gui --session-selftest failed (rc={ran.returncode}):\n"
                    f"{ran.stdout.strip()}\n{ran.stderr.strip()}")
    print(ran.stdout.strip().splitlines()[-1] if ran.stdout.strip() else "session selftest passed")
    return 0


# The capture-metadata descriptor order. The session parser, the capture-log
# block, the sidecar's asset object and the panel's rows all walk this one
# table, so its order is the log's line order and the sidecar's key order --
# what downstream readers (the toolkit) see. Change it deliberately or not at all.
CAPTURE_META_FIELD_ORDER = (
    "client_name", "display_name", "index", "label", "format", "tape_speed",
    "video_system", "hifi_audio_equipped", "black_and_white", "notes",
    "asset_id", "client_id", "operator",
)
CAPTURE_META_ASSET_KEYS = CAPTURE_META_FIELD_ORDER[:-1]   # operator is top-level
CAPTURE_META_SIDECAR_KEYS = (
    "schema", "state", "linked", "asset", "operator", "operator_source", "session_file",
    "misrc_gui_version", "computer_name", "device", "capture_format", "started_at",
    "ended_at", "capture_seconds", "output_path", "base_name", "log_file", "rf_channels", "files",
    "result",
)
# The two capture-log lines a --session "rf_channels" request adds (only then).
CAPTURE_META_RF_LOG_KEYS = ("rf_channels_requested", "rf_channels_recorded")
CAPTURE_META_AUDIO_KEYS = (
    "audio_4ch", "audio_2ch_12", "audio_2ch_34",
    "audio_1ch_1", "audio_1ch_2", "audio_1ch_3", "audio_1ch_4",
)


def check_capture_metadata_contract(repo_root: Path) -> int:
    """Capture metadata (what a capture is OF) lives outside the settings, in
    core/gui_capture_meta.c, and reaches three outputs that other programs
    read: the capture log's "Capture metadata <key>: <value>" block, the
    {base}_{dateTag}_capture_meta.json sidecar next to it, and the RF FLAC
    tags written at encoder init. This pins the parts a reader depends on --
    the descriptor order, the literals, the tag names, the shared stem -- and
    the two refusals that keep a linked capture honest: a net client that
    records on the server will not start, and the panel locks while recording."""
    base = repo_root / "misrc_tools/misrc_gui"
    try:
        meta_c = strip_c_comments(read_text(base / "core/gui_capture_meta.c"))
        record_c = strip_c_comments(read_text(base / "output/gui_record.c"))
        sidecar_h = read_text(base / "output/gui_capture_sidecar.h")
        sidecar_c = strip_c_comments(read_text(base / "output/gui_capture_sidecar.c"))
        capture_c =strip_c_comments(read_text(base / "input/gui_capture.c"))
        panel_c = strip_c_comments(read_text(base / "ui/gui_capture_meta_panel.c"))
        ui_c = strip_c_comments(read_text(base / "ui/gui_ui.c"))
        flac_c = strip_c_comments(read_text(repo_root / "misrc_tools/common/flac_writer.c"))
        meson = read_text(repo_root / "misrc_tools/meson.build")
    except OSError as exc:
        return fail(f"capture metadata guard: cannot read a source file: {exc}")

    table = meta_c[meta_c.find("s_fields[] = {"):]
    table = table[:table.find("};")]
    keys = re.findall(r'(?:META_STR_FIELD\(|\{)\s*"([a-z_]+)"', table)
    if tuple(keys) != CAPTURE_META_FIELD_ORDER:
        return fail("gui_capture_meta.c: the descriptor order changed:\n"
                    f"  now:    {', '.join(keys)}\n  pinned: {', '.join(CAPTURE_META_FIELD_ORDER)}\n"
                    "It is the capture log's line order and the sidecar's key order; downstream "
                    "readers see it. Update CAPTURE_META_FIELD_ORDER only on purpose.")

    for needle, why in (
        ('"Capture metadata %s: %s"', "the log block's line format"),
        ('_misrc_capture.log', "the capture log's name (the sidecar shares its stem)"),
        ('"MISRC_ASSET_ID"', "the RF FLAC asset tag"),
        ('"MISRC_RF_CHANNEL"', "the RF FLAC channel tag"),
        ('"MISRC_CAPTURE_META"', "the RF FLAC tag naming the sidecar"),
        ("gui_record_build_capture_stem(", "the one stem the log and the sidecar share"),
        ("initial_tags", "FLAC tags written at encoder init"),
    ):
        if needle not in record_c:
            return fail(f"gui_record.c: {needle} is gone ({why})")
    # (The selftest names "Ingest metadata" to assert its absence; a format
    # string that writes the line is what must not come back.)
    if re.search(r'"Ingest metadata [^"]*%s', record_c):
        return fail("gui_record.c: the retired \"Ingest metadata\" lines are back; the block is "
                    "\"Capture metadata <key>: <value>\" from the descriptor table")
    for needle in ('"_capture_meta.json"', '"misrc-gui.capture-meta/1"'):
        if needle not in sidecar_h:
            return fail(f"gui_capture_sidecar.h: {needle} is gone (the sidecar's name/schema)")
    # rf_channels: a --session's request beside what the recording used.
    for key in CAPTURE_META_RF_LOG_KEYS:
        if f'"Capture metadata %s: A=%s B=%s", "{key}"' not in record_c:
            return fail(f"gui_record.c: the \"Capture metadata {key}: A=on|off B=on|off\" log line is "
                        "gone (the toolkit reads the rf_channels request against what was recorded)")
    for needle in ('"rf_channels"', '{\\"requested\\": ', '\\"recorded\\": {\\"a\\": '):
        if needle not in sidecar_c:
            return fail(f"gui_capture_sidecar.c: {needle} is gone (the sidecar's rf_channels key: "
                        "requested {a, b} | null, recorded {a, b})")
    if "initial_tags" not in flac_c or "initial_tags_dropped" not in flac_c:
        return fail("common/flac_writer.c: the initial_tags hunk is gone -- the capture-metadata FLAC "
                    "tags would only be written at finalize, and a crash would lose the link. Keep "
                    "the fork's hunk on upstream syncs.")

    start = capture_c.find("int gui_app_start_recording(gui_app_t *app)")
    body = capture_c[start:capture_c.find("\n}", start)] if start >= 0 else ""
    fwd = body.find("gui_net_client_request_record(app, true)")
    linked = body.find("linked")
    if fwd < 0 or linked < 0 or linked > fwd or "gui_popup_info(" not in body:
        return fail("gui_capture.c: gui_app_start_recording() must refuse (with a popup) to forward "
                    "Record to the server while the seat is linked -- the server's files would not "
                    "carry the link")

    start = panel_c.find("bool gui_capture_meta_panel_can_edit(")
    can_edit = panel_c[start:panel_c.find("\n}", start)] if start >= 0 else ""
    if "gui_app_effective_recording(" not in can_edit or "linked" not in can_edit:
        return fail("gui_capture_meta_panel.c: can_edit must lock while linked and while "
                    "gui_app_effective_recording() (the snapshot taken at start is what the files carry)")

    start = ui_c.find("static void gui_ui_commit_text_edit(")
    commit = ui_c[start:ui_c.find("\n}", start)] if start >= 0 else ""
    meta_at = commit.find("gui_ui_is_meta_field(")
    ret_at = commit.find("return;", meta_at)
    save_at = commit.find("gui_settings_save(")
    if meta_at < 0 or ret_at < 0 or save_at < 0 or not (meta_at < ret_at < save_at):
        return fail("gui_ui.c: a capture-metadata edit must return before gui_settings_save() in "
                    "gui_ui_commit_text_edit -- the values are per-run and never saved")
    if "settings.ingest_" in ui_c or "UI_TEXT_FIELD_INGEST" in ui_c or "render_metadata_window" in ui_c:
        return fail("gui_ui.c: the retired ingest metadata window is back; the panel lives in "
                    "ui/gui_capture_meta_panel.c")

    for src in ("'misrc_gui/core/gui_capture_meta.c'", "'misrc_gui/output/gui_capture_sidecar.c'",
                "'misrc_gui/ui/gui_capture_meta_panel.c'"):
        if src not in meson:
            return fail(f"meson.build: {src} is not built")
    return 0


def _check_sidecar_json(path: Path, data: object) -> Optional[str]:
    """Key set and types of one capture-meta sidecar; None when it is valid."""
    if not isinstance(data, dict):
        return "not a JSON object"
    if tuple(data.keys()) != CAPTURE_META_SIDECAR_KEYS:
        return f"top-level keys {list(data.keys())} != {list(CAPTURE_META_SIDECAR_KEYS)}"
    if data["schema"] != "misrc-gui.capture-meta/1":
        return f"schema {data['schema']!r}"
    if data["state"] not in ("recording", "complete"):
        return f"state {data['state']!r}"
    if not isinstance(data["linked"], bool):
        return "linked is not a bool"
    asset = data["asset"]
    if not isinstance(asset, dict) or tuple(asset.keys()) != CAPTURE_META_ASSET_KEYS:
        return f"asset keys {list(asset.keys()) if isinstance(asset, dict) else asset!r}"
    for k, v in asset.items():
        if k == "index":
            ok = v is None or (isinstance(v, int) and not isinstance(v, bool) and v >= 0)
        elif k in ("hifi_audio_equipped", "black_and_white"):
            ok = v is None or isinstance(v, bool)
        else:
            ok = isinstance(v, str)
        if not ok:
            return f"asset.{k} has the wrong type: {v!r}"
    if data["linked"] and not all(asset[k] for k in ("asset_id", "client_id", "client_name",
                                                      "display_name", "label", "format")):
        return "linked but a required asset field is empty"
    if not isinstance(data["operator"], str) or not data["operator"]:
        return "operator is not a non-empty string"
    if data["operator_source"] not in ("session", "os_login"):
        return f"operator_source {data['operator_source']!r}"
    for k in ("session_file",):
        if data[k] is not None and not isinstance(data[k], str):
            return f"{k} is not a string or null"
    for k in ("misrc_gui_version", "computer_name", "capture_format", "output_path", "base_name", "log_file"):
        if not isinstance(data[k], str):
            return f"{k} is not a string"
    rf = data["rf_channels"]
    if not isinstance(rf, dict) or tuple(rf.keys()) != ("requested", "recorded"):
        return f"rf_channels {rf!r}"
    for k in ("requested", "recorded"):
        pair = rf[k]
        if k == "requested" and pair is None:
            continue
        if not isinstance(pair, dict) or tuple(pair.keys()) != ("a", "b") or \
                not all(isinstance(v, bool) for v in pair.values()):
            return f"rf_channels.{k} {pair!r}"
    if data["capture_format"] not in ("FLAC", "RAW"):
        return f"capture_format {data['capture_format']!r}"
    dev = data["device"]
    if not isinstance(dev, dict) or set(dev) != {"name", "type"} or not all(isinstance(x, str) for x in dev.values()):
        return f"device {dev!r}"
    stamp = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
    if not isinstance(data["started_at"], str) or not stamp.match(data["started_at"]):
        return f"started_at {data['started_at']!r}"
    complete = data["state"] == "complete"
    if complete:
        if not isinstance(data["ended_at"], str) or not stamp.match(data["ended_at"]):
            return f"complete but ended_at {data['ended_at']!r}"
        if not isinstance(data["capture_seconds"], (int, float)) or isinstance(data["capture_seconds"], bool):
            return f"complete but capture_seconds {data['capture_seconds']!r}"
    files = data["files"]
    if not isinstance(files, dict) or tuple(files.keys()) != ("rf_a", "rf_b", "video", "closed_captions", "audio"):
        return f"files keys {list(files.keys()) if isinstance(files, dict) else files!r}"
    names = [data["log_file"]]
    for ch in ("rf_a", "rf_b"):
        rf = files[ch]
        if rf is None:
            continue
        if not isinstance(rf, dict) or tuple(rf.keys()) != ("name", "bits", "sample_rate_hz", "samples", "bytes"):
            return f"files.{ch} keys {rf!r}"
        if not isinstance(rf["name"], str) or not isinstance(rf["bits"], int) or not isinstance(rf["sample_rate_hz"], int):
            return f"files.{ch} types {rf!r}"
        for k in ("samples", "bytes"):
            if rf[k] is not None and not isinstance(rf[k], int):
                return f"files.{ch}.{k} {rf[k]!r}"
            if complete and rf[k] is None:
                return f"complete but files.{ch}.{k} is null"
        names.append(rf["name"])
        if complete and not (path.parent / rf["name"]).is_file():
            return f"files.{ch}.name {rf['name']!r} is not a file next to the sidecar"
        if complete and (path.parent / rf["name"]).stat().st_size != rf["bytes"]:
            return f"files.{ch}.bytes does not match the file on disk"
    for k in ("video", "closed_captions"):
        if files[k] is not None and not isinstance(files[k], str):
            return f"files.{k} {files[k]!r}"
        names.append(files[k])
    audio = files["audio"]
    if not isinstance(audio, dict) or tuple(audio.keys()) != CAPTURE_META_AUDIO_KEYS:
        return f"files.audio keys {audio!r}"
    for k, v in audio.items():
        if v is not None and not isinstance(v, str):
            return f"files.audio.{k} {v!r}"
        names.append(v)
    for n in names:
        if n is not None and ("/" in n or "\\" in n):
            return f"{n!r} is not a basename"
    if not (path.parent / data["log_file"]).is_file():
        return f"log_file {data['log_file']!r} is not next to the sidecar"
    if not path.name.endswith("_capture_meta.json") or \
            data["log_file"][:-len("_misrc_capture.log")] != path.name[:-len("_capture_meta.json")]:
        return "the sidecar and the log do not share a stem"
    result = data["result"]
    if complete:
        if not isinstance(result, dict) or set(result) != {"drops", "waits"} or \
                not all(isinstance(x, int) for x in result.values()):
            return f"complete but result {result!r}"
    elif result is not None:
        return f"recording but result {result!r}"
    return None


def check_capture_metadata_post_build(gui_path: Path) -> int:
    """Run the built GUI's --capture-meta-selftest (a linked FLAC A+B and an
    unlinked RAW recording from the Simulated device; it asserts the sidecar,
    the log block and the FLAC tags itself), then json.loads every sidecar it
    left and checks the key set and every type -- the contract a downstream
    reader parses against."""
    if gui_path.suffix == ".exe":
        print("SKIP: capture metadata selftest (post-build) on Windows")
        return 0
    with tempfile.TemporaryDirectory(prefix="misrc_capture_meta_guard_") as temp_root:
        out = Path(temp_root) / "out"
        env = dict(os.environ, TMPDIR=temp_root)
        try:
            ran = subprocess.run([str(gui_path), "--capture-meta-selftest", str(out)],
                                 capture_output=True, text=True, timeout=240, env=env)
        except subprocess.TimeoutExpired:
            return fail("misrc_gui --capture-meta-selftest timed out")
        if ran.returncode == 2 or "no simulated device" in ran.stderr:
            print("SKIP: capture metadata selftest (post-build): the Simulated device did not run "
                  f"headless here (rc={ran.returncode}): {ran.stderr.strip().splitlines()[-1:]}")
            return 0
        if ran.returncode != 0:
            return fail(f"misrc_gui --capture-meta-selftest failed (rc={ran.returncode}):\n"
                        f"{ran.stdout.strip()}")
        sidecars = sorted(out.rglob("*_capture_meta.json"))
        if len(sidecars) != 2:
            return fail(f"--capture-meta-selftest left {len(sidecars)} sidecar(s), expected 2 "
                        "(linked + unlinked)")
        for sidecar in sidecars:
            try:
                data = json.loads(sidecar.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                return fail(f"{sidecar.name} is not valid JSON: {exc}")
            problem = _check_sidecar_json(sidecar, data)
            if problem:
                return fail(f"{sidecar.parent.name}/{sidecar.name}: {problem}")
        loaded = [json.loads(p.read_text(encoding="utf-8")) for p in sidecars]
        linked = [d["linked"] for d in loaded]
        if sorted(linked) != [False, True]:
            return fail(f"expected one linked and one unlinked sidecar, got linked={linked}")
        # The linked run's session asked for A only and the run recorded A+B;
        # the unlinked run had no request.
        for d in loaded:
            want = ({"requested": {"a": True, "b": False}, "recorded": {"a": True, "b": True}}
                    if d["linked"] else {"requested": None, "recorded": {"a": True, "b": True}})
            if d["rf_channels"] != want:
                return fail(f"{'linked' if d['linked'] else 'unlinked'} sidecar rf_channels "
                            f"{d['rf_channels']!r} != {want!r}")
        leftovers = [p.name for p in out.rglob("*.tmp.*")]
        if leftovers:
            return fail(f"sidecar temp files left behind: {leftovers}")
    print(ran.stdout.strip().splitlines()[-1] if ran.stdout.strip() else "capture-meta selftest passed")
    return 0


def check_audio_record_alignment_contract(repo_root: Path) -> int:
    """Static contract for the F1 record-alignment transitions in the
    audio thread: transitions detected at the top of the loop (bounded by
    the 2 ms read timeout, so within ~2-4 ms of the record button), a
    discard-to-now of buffered pre-record audio at record START at the
    576 B producer granularity, and a flush-to-now of buffered tail audio
    at record STOP (the old semantics wrote up to one full ~350 ms block
    of pre-record audio at record START)."""
    audio_c = read_text(repo_root / "misrc_tools/misrc_gui/output/gui_audio.c")
    buffer_manager_c = read_text(repo_root / "misrc_tools/common/buffer_manager.c")

    required_snippets = [
        (audio_c, "#define AUDIO_SYNC_CHUNK_BYTES 576", "producer-granularity sync chunk (48 frames x 12 B)"),
        (audio_c, "static uint64_t audio_discard_ring_to_now(", "discard-to-now helper"),
        (audio_c, "static uint64_t audio_flush_ring_to_files(", "flush-to-now helper"),
        (audio_c, "static void audio_write_recorded_block(", "shared block-write helper"),
        (audio_c, "BUF_CAPTURE_AUDIO,\n                                    AUDIO_SYNC_CHUNK_BYTES, 0)", "sync helpers use no-wait full chunks"),
        (audio_c, "[AUDIO] Record start aligned", "start-alignment log line"),
        (audio_c, "[AUDIO] Record stop aligned", "stop-alignment log line"),
        (audio_c, "bool was_recording = (a->f_4ch != NULL)", "pre-opened-files recording init"),
        (audio_c, "bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);", "2 ms read timeout keeps the transition observation at ~2-4 ms"),
        (buffer_manager_c, "if (timeout_ms == 0) return NULL;", "timeout-0 read returns only full available chunks"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"Audio record alignment contract missing {label}: {snippet}")

    # The transition handler must sit at the TOP of the audio loop, before
    # the main full-block read, so transitions are observed within the
    # 10 ms read-timeout spin instead of only at the next full ~350 ms block.
    thread_body = func_body_from(audio_c, "static int audio_thread_main(void *ctx)")
    loop_pos = thread_body.find("while (1) {")
    transition_pos = thread_body.find("bool rec_now = a->app && a->app->is_recording;")
    read_pos = thread_body.find("bufmgr_read_begin(a->bufmgr, BUF_CAPTURE_AUDIO, len, 2);")
    if min(loop_pos, transition_pos, read_pos) < 0:
        return fail("Audio record alignment anchors not found in audio_thread_main")
    if not (loop_pos < transition_pos < read_pos):
        return fail("Audio record transition handler must sit at the top of the audio loop, before the main full-block read")

    # START discards, STOP flushes: both through the producer-granularity
    # chunk (12,288 B = one clockgen capture-thread write) and both before
    # the main full-block read path runs.
    start_discard_pos = thread_body.find("audio_discard_ring_to_now(a);", transition_pos)
    stop_flush_pos = thread_body.find("audio_flush_ring_to_files(a);", transition_pos)
    if start_discard_pos < 0 or stop_flush_pos < 0:
        return fail("Audio record transitions must call the discard/flush helpers")
    if not (start_discard_pos < read_pos and stop_flush_pos < read_pos):
        return fail("Audio record transition handlers must run before the main full-block read")

    # The flush must write through the same conversion path (peaks + block
    # writer) rather than dropping the tail.
    flush_body = func_body_from(audio_c, "static uint64_t audio_flush_ring_to_files")
    for snippet in ["audio_update_peaks", "audio_write_recorded_block", "bufmgr_read_end"]:
        if snippet not in flush_body:
            return fail(f"audio_flush_ring_to_files must use the shared write path: missing {snippet}")
    return 0


def check_cxadc_sync_start_contract(repo_root: Path) -> int:
    """Static contract for the F1b synchronized card starts: after the
    sequential capture-start rate probes (which leave card 0's stream ~one
    probe duration behind card 1's in wall-clock content time), both cards
    are drained to their current 2 MiB release boundary and skipped to a
    common content instant T_ref BEFORE the RF lockstep thread starts, so
    the recorded A/B content offset is bounded by read granularity and
    measurement slop instead of the probe skew (~114 ms measured)."""
    cxadc_c = read_text(repo_root / "misrc_tools/misrc_gui/input/gui_cxadc.c")

    required_snippets = [
        (cxadc_c, "static void cxadc_sync_card_starts(", "synchronized-start routine"),
        (cxadc_c, "#define CXADC_SYNC_CHUNK_BYTES", "sync chunk granularity"),
        (cxadc_c, "#define CXADC_SYNC_LIVE_CHUNK_MIN_US", "live-detection threshold"),
        (cxadc_c, "#define CXADC_SYNC_GRID_LIVE_MIN_US", "grid-read live-detection threshold"),
        (cxadc_c, "#define CXADC_SYNC_TREF_MARGIN_US", "T_ref margin"),
        (cxadc_c, "#define CXADC_SYNC_MAX_DRAIN_BYTES", "drain byte cap"),
        (cxadc_c, "#define CXADC_SYNC_MAX_TOTAL_US", "total sync budget"),
        (cxadc_c, "static size_t s_cxadc_probe_consumed_bytes[CXADC_MAX_CARDS];", "per-session consumed grid reference"),
        (cxadc_c, "[CXADC] sync-start:", "sync-start log line"),
    ]
    for text, snippet, label in required_snippets:
        if snippet not in text:
            return fail(f"CXADC synchronized start contract missing {label}: {snippet}")

    sync_body = func_body_from(cxadc_c, "static void cxadc_sync_card_starts")
    for snippet in [
        "CXADC_SYNC_LIVE_CHUNK_MIN_US",          # phase-1 drain live heuristic
        "% CXADC_SYNC_RELEASE_BYTES",           # grid-align to the 2 MiB boundary grid
        "rem - (size_t)got",                    # exact-remainder consumption (no overshoot)
        "CXADC_SYNC_GRID_LIVE_MIN_US",          # grid-read full-period wait detection
        "cxadc_sync_read_card(ctx, i, buf, 1)",  # 1-byte boundary-crossing probe
        "probe_us >= 1000",                     # probe must truly block (crossing measured)
        "t_ref += CXADC_SYNC_TREF_MARGIN_US",   # common target with margin
        "delta_us * byte_rate",                 # skip bytes from content-time delta
        "card_sample_rate_hz[i]",               # per-card measured rate
        "tenbit_mode[i]",                       # per-card sample width
        "s_cxadc_probe_consumed_bytes[i]",       # grid reference from the probe's consumed count
    ]:
        if snippet not in sync_body:
            return fail(f"cxadc_sync_card_starts missing required logic: {snippet}")

    # The sync must run AFTER the capture-start probe loop and BEFORE the RF
    # capture thread is created, gated on two or more cards.
    start_body = func_body_from(
        cxadc_c, "int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)")
    probe_pos = start_body.find("cxadc_probe_and_cache_card_rate(app, i, s_cxadc.tenbit_mode[i],")
    sync_call_pos = start_body.find("cxadc_sync_card_starts(&s_cxadc, open_count);")
    gate_pos = start_body.find("if (open_count > 1) {")
    rf_thread_pos = start_body.find("thrd_create_with_priority(&s_cxadc.rf_thread,")
    if min(probe_pos, sync_call_pos, gate_pos, rf_thread_pos) < 0:
        return fail("CXADC sync-start ordering anchors not found in gui_cxadc_start")
    if not (probe_pos < sync_call_pos < rf_thread_pos):
        return fail("cxadc_sync_card_starts must run after the capture-start probes and before the RF thread starts")
    if not (gate_pos < sync_call_pos):
        return fail("cxadc_sync_card_starts must be gated on open_count > 1 (the cards this capture opened)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="MISRC CI guard tests")
    parser.add_argument(
        "--static-only",
        action="store_true",
        help="Run static/text invariants only (skip runtime AppRun simulation)",
    )
    parser.add_argument(
        "--post-build",
        action="store_true",
        help="Post-build mode: also run binary-introspection guards (hsdaoh linkage, "
             "FX3 symbols) against --gui-path. Used by CI build jobs after misrc_gui is built.",
    )
    parser.add_argument(
        "--gui-path",
        type=Path,
        default=None,
        help="Path to the built misrc_gui binary for --post-build binary-introspection checks.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    workflow_path = repo_root / ".github/workflows/build.yml"
    legacy_workflow_path = repo_root / ".github/workflows/release-sanity-build.yml"
    gui_c_path = repo_root / "misrc_tools/misrc_gui/core/misrc_gui.c"
    gui_settings_c_path = repo_root / "misrc_tools/misrc_gui/core/gui_settings.c"
    gui_ui_c_path = repo_root / "misrc_tools/misrc_gui/ui/gui_ui.c"
    flac_writer_c_path = repo_root / "misrc_tools/common/flac_writer.c"
    meson_path = repo_root / "misrc_tools/meson.build"
    tools_readme_path = repo_root / "misrc_tools/README.md"
    installation_md_path = repo_root / "INSTALLATION.md"
    dev_notes_path = repo_root / "misrc_tools/misrc_gui/dev/dev_notes_README.md"
    icon_path = repo_root / "assets/Icons/MISRC_Icon.png"

    checks: List[Tuple[str, Callable[[], int]]] = [
        ("cross-platform workflow coverage", lambda: check_cross_platform_workflow_coverage(workflow_path)),
        ("actions runtime policy", lambda: check_actions_runtime_policy(workflow_path)),
        ("macOS brew install policy", lambda: check_macos_brew_install_policy(workflow_path)),
        ("workflow FFT dependency policy", lambda: check_workflow_fft_dependency_policy(workflow_path)),
        ("MSYS2 toolchain policy", lambda: check_msys2_toolchain_policy(workflow_path)),
        ("libc direct-include contract", lambda: check_libc_direct_include_contract(repo_root)),
        ("meson FFT policy", lambda: check_meson_fft_policy(meson_path)),
        ("meson vendored hsdaoh policy", lambda: check_meson_vendored_hsdaoh_policy(meson_path)),
        ("meson FX3 native-build policy", lambda: check_meson_fx3_policy(meson_path)),
        ("cross-platform smoke tests", lambda: check_cross_platform_smoke_tests(workflow_path)),
        ("linux desktop metadata", lambda: check_linux_desktop_metadata(repo_root, workflow_path)),
        ("window identity contract", lambda: check_window_identity_contract(repo_root)),
        ("WM_CLASS matches GUI window class", lambda: check_wm_class_consistency(gui_c_path, repo_root)),
        ("native Wayland build and window platform", lambda: check_native_wayland_contract(repo_root)),
        ("macOS layout policy", lambda: check_macos_layout_policy(gui_ui_c_path)),
        ("macOS startup admin elevation contract", lambda: check_macos_admin_elevation_contract(gui_c_path)),
        ("Windows meson subsystem contract", lambda: check_windows_meson_subsystem_contract(meson_path)),
        ("dev version naming", lambda: check_dev_version_naming(repo_root, meson_path, workflow_path)),
        ("no tracked generated dirty sources", lambda: check_no_tracked_generated_dirty_sources(repo_root)),
        ("Windows GUI link no DLL import libs", lambda: check_windows_gui_link_no_dll_import_libs(meson_path)),
        ("optional-dep guard consistency", lambda: check_optional_dep_guard_consistency(repo_root)),
        ("debug-view runtime contract", lambda: check_debug_view_contract(gui_c_path)),
        ("settings persistence contract", lambda: check_settings_persistence_contract(gui_settings_c_path)),
        ("UI scale integration contract", lambda: check_ui_scale_integration_contract(
            repo_root, gui_c_path, gui_settings_c_path, meson_path)),
        ("FLAC large-file offsets contract", lambda: check_flac_large_file_offsets_contract(flac_writer_c_path)),
        ("preview tap single-slot contract", lambda: check_preview_tap_single_slot_contract(repo_root)),
        ("alsa never stores a card index", lambda: check_alsa_never_stores_a_card_index(repo_root)),
        ("cxadc audio probe prefers a stable card id",
         lambda: check_cxadc_audio_probe_prefers_stable_card_id(repo_root)),
        ("bundled mediamtx contract", lambda: check_bundled_mediamtx_contract(repo_root)),
        ("rtsp settings round-trip", lambda: check_rtsp_settings_roundtrip(repo_root)),
        ("bitrate stepper survives a reload", lambda: check_bitrate_stepper_survives_a_reload(repo_root)),
        ("stream password is strong and unleaked", lambda: check_stream_password_is_strong_and_unleaked(repo_root)),
        ("LAN requires acknowledgement", lambda: check_lan_requires_acknowledgement(repo_root)),
        ("live readout cannot resize the panel", lambda: check_live_stream_readout_cannot_resize_the_panel(repo_root)),
        ("streaming children yield to RF", lambda: check_streaming_children_yield_to_rf(repo_root)),
        ("capture children I/O class", lambda: check_capture_children_io_class(repo_root)),
        ("GDH host installer", lambda: check_gdh_host_installer(repo_root)),
        ("streaming writes are private", lambda: check_streaming_writes_are_private(repo_root)),
        ("Clay text outlives the layout pass", lambda: check_clay_text_outlives_layout(repo_root)),
        ("URL opening is whitelisted", lambda: check_url_open_is_whitelisted(repo_root)),
        ("net controls publish the effective mode", lambda: check_net_controls_publish_effective_mode(repo_root)),
        ("caption recorder is a subprocess and nothing else", lambda: check_cc_record_is_subprocess_only(repo_root)),
        ("v4l2vbi is probed, never assumed", lambda: check_cc_record_probes_never_assumes(repo_root)),
        ("optional-output preflights refuse before any file is opened", lambda: check_cc_preflight_refuses_before_files(repo_root)),
        ("caption sidecar is in the overwrite set", lambda: check_cc_sidecar_in_overwrite_set(repo_root)),
        ("no record start while finalizing", lambda: check_record_start_refuses_while_finalizing(repo_root)),
        ("caption settings have defaults and table rows", lambda: check_cc_settings_have_defaults_and_rows(repo_root)),
        ("every settings field is in the table", lambda: check_settings_table_covers_struct(repo_root)),
        ("net settings protocol contract", lambda: check_net_settings_protocol(repo_root)),
        ("record button follows the effective recording state", lambda: check_record_parity(repo_root)),
        ("AppRun static contract", lambda: check_apprun_static_contract(repo_root)),
        ("Windows packaging assertions", lambda: check_windows_packaging_assertions(workflow_path)),
        ("Android packaging assertions", lambda: check_android_packaging_assertions(workflow_path)),
        ("release artifact naming contract", lambda: check_release_artifact_naming_contract(repo_root, workflow_path)),
        ("release download link contract", lambda: check_release_download_link_contract(repo_root, workflow_path)),
        ("release version resolution contract", lambda: check_release_version_resolution_contract(repo_root, workflow_path)),
        ("build workflow entrypoint contract", lambda: check_build_workflow_entrypoint_contract(workflow_path)),
        ("CI-only tier contract",
         lambda: check_ci_only_tier_contract(repo_root / ".github/workflows/selfhosted-deploy.yml")),
        ("legacy release-sanity workflow removed", lambda: check_no_legacy_release_sanity_workflow(legacy_workflow_path)),
        ("no capture-stability Actions clutter", lambda: check_no_capture_stability_clutter(workflow_path)),
        ("local build bootstrap contract", lambda: check_local_build_bootstrap_contract(repo_root, dev_notes_path, installation_md_path)),
        ("local deps cache contract", lambda: check_local_deps_cache_contract(repo_root, workflow_path, dev_notes_path, installation_md_path)),
        ("raw direct passthrough contract", lambda: check_raw_direct_passthrough_contract(repo_root)),
        ("GUI auto-test flags contract", lambda: check_gui_auto_test_flags(repo_root)),
        ("CXADC rate probe contract", lambda: check_cxadc_rate_probe_contract(repo_root)),
        ("audio record alignment contract", lambda: check_audio_record_alignment_contract(repo_root)),
        ("CXADC synchronized start contract", lambda: check_cxadc_sync_start_contract(repo_root)),
        ("CXADC card B stays closed when RF B is off", lambda: check_cxadc_skips_card_b_when_rf_b_off(repo_root)),
        ("pane menu and per-pane source", lambda: check_pane_menu_contract(repo_root)),
        ("channel gear clears the panel labels", lambda: check_channel_gear_clearance(repo_root)),
        ("preview crop never reaches a recording", lambda: check_preview_crop_never_reaches_a_recording(repo_root)),
        ("USB reference video UI stays out of gui_ui.c", lambda: check_usbref_ui_stays_out_of_gui_ui(repo_root)),
        ("desktop-sized cursor", lambda: check_desktop_sized_cursor(repo_root)),
        ("About dialog never scrolls sideways", lambda: check_about_dialog_never_scrolls_sideways(repo_root)),
        ("UI zoom is relative to the desktop", lambda: check_ui_zoom_is_desktop_relative(repo_root)),
        ("session launch file contract", lambda: check_session_launch_file_contract(repo_root)),
        ("capture metadata contract", lambda: check_capture_metadata_contract(repo_root)),
    ]
    if not args.static_only:
        checks.insert(7, ("AppRun runtime behavior", lambda: check_apprun_runtime_behavior(repo_root, icon_path)))
        checks.insert(8, ("record ringbuffer fallback runtime", lambda: check_record_ringbuffer_fallback_runtime(repo_root)))
        checks.insert(9, ("ringbuffer mirror runtime", lambda: check_ringbuffer_mirror_runtime(repo_root)))
        checks.insert(10, ("net fanout runtime", lambda: check_net_fanout_runtime(repo_root)))
        checks.insert(11, ("buffer manager write tap runtime", lambda: check_bufmgr_write_tap_runtime(repo_root)))
        checks.insert(12, ("settings round-trip runtime", lambda: check_settings_roundtrip_runtime(repo_root)))
        checks.insert(13, ("net query runtime", lambda: check_net_query_runtime(repo_root)))
        checks.insert(9, ("UI scale policy runtime", lambda: check_ui_scale_policy_runtime(repo_root)))
        checks.insert(10, ("FLAC STREAMINFO total_samples runtime", lambda: check_flac_streaminfo_total_samples_runtime(repo_root)))
        checks.insert(11, ("preview tap mux runtime", lambda: check_preview_tap_mux_runtime(repo_root)))
        checks.insert(12, ("mediamtx config runtime", lambda: check_mediamtx_config_runtime(repo_root)))
        checks.insert(13, ("alsa device resolution", lambda: check_alsa_device_resolution(repo_root)))
        checks.insert(14, ("closed-caption argv contract", lambda: check_cc_record_argv_runtime(repo_root)))
        checks.insert(15, ("closed-caption child I/O class", lambda: check_cc_record_ioprio_runtime(repo_root)))
        checks.insert(15, ("priority clamp runtime", lambda: check_priority_clamp_runtime(repo_root)))
        checks.insert(16, ("FLAC worker priority runtime", lambda: check_flac_worker_priority_runtime(repo_root)))
        checks.insert(17, ("SDTV preview geometry runtime", lambda: check_preview_sdtv_geometry_runtime(repo_root)))
        checks.insert(10, ("direct RAW writer runtime", lambda: check_raw_direct_writer_runtime(repo_root)))
        checks.insert(11, ("built GUI links vendored hsdaoh", lambda: check_built_gui_links_vendored_hsdaoh(repo_root, args.gui_path)))
    # --post-build: always run the binary-introspection guards against the real
    # built misrc_gui (passed via --gui-path by CI build jobs). This is the mode
    # that catches vendored-dep shadowing and silent FX3-disable on every build.
    if args.post_build:
        if args.gui_path is None:
            return fail("--post-build requires --gui-path pointing at the built misrc_gui")
        # In post-build mode the binary MUST exist: a missing misrc_gui means the
        # build step failed or the path is wrong, and silently skipping would let
        # CI pass without ever validating vendored-dep linkage or FX3 compilation.
        # (Preflight mode above still skips gracefully when no local build exists.)
        if not args.gui_path.exists():
            return fail(f"--post-build --gui-path does not exist (build did not produce misrc_gui?): {args.gui_path}")
        checks.append(("built GUI links vendored hsdaoh (post-build)", lambda: check_built_gui_links_vendored_hsdaoh(repo_root, args.gui_path)))
        checks.append(("built GUI has FX3 symbols (post-build)", lambda: check_built_gui_has_fx3_symbols(repo_root, args.gui_path)))
        checks.append(("per-pane source routing harness (post-build)", lambda: check_panel_source_harness_post_build(args.gui_path)))
        checks.append(("session launch file selftest (post-build)", lambda: check_session_launch_file_post_build(args.gui_path)))
        checks.append(("capture metadata selftest (post-build)", lambda: check_capture_metadata_post_build(args.gui_path)))
        checks.append(("record gate selftest (post-build)", lambda: check_record_gate_post_build(args.gui_path)))

    for name, check in checks:
        rc = check()
        if rc is None:
            # A check that falls off its end returns None, and SystemExit(None)
            # is a SUCCESSFUL exit -- so the suite would print FAILED and still
            # go green. Treat it as the bug it is.
            print(f"FAILED: {name} (check returned None; it is missing a return)",
                  file=sys.stderr)
            return 1
        if rc != 0:
            print(f"FAILED: {name}", file=sys.stderr)
            return rc if isinstance(rc, int) and rc != 0 else 1
        print(f"PASS: {name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
