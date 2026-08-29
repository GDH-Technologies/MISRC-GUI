#!/usr/bin/env python3
import argparse
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


GUI_WINDOW_CLASS_DEFINE = re.compile(r'^#define\s+GUI_WINDOW_CLASS_NAME\s+"([^"]+)"', re.M)

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
    """Blank out /* */ and // comments, preserving line structure."""
    out = []
    i, n = 0, len(source)
    while i < n:
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
        else:
            out.append(source[i])
            i += 1
    return "".join(out)


def read_gui_window_class_name(gui_c_path: Path) -> str:
    """The one source of truth for WM_CLASS. misrc_gui.c creates its window under
    this exact name, so every .desktop we generate must set StartupWMClass to it."""
    match = GUI_WINDOW_CLASS_DEFINE.search(read_text(gui_c_path))
    if not match:
        raise RuntimeError("Could not find #define GUI_WINDOW_CLASS_NAME in misrc_gui.c")
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


def extract_apprun_script(workflow_text: str) -> str:
    marker = "cat > AppDir/AppRun <<'EOF'"
    start = workflow_text.find(marker)
    if start < 0:
        raise RuntimeError("Could not find AppRun heredoc start marker in workflow")
    start = workflow_text.find("\n", start)
    if start < 0:
        raise RuntimeError("Malformed AppRun heredoc in workflow")
    start += 1
    end = workflow_text.find("\n          EOF", start)
    if end < 0:
        raise RuntimeError("Could not find AppRun heredoc end marker in workflow")
    return textwrap.dedent(workflow_text[start:end]).lstrip("\n")


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
def check_workflow_fft_dependency_policy(workflow_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    required_snippets = [
        "libfftw3-dev",
        "mingw-w64-x86_64-fftw",
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


def check_linux_desktop_metadata(workflow_path: Path, gui_c_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    wm_class = read_gui_window_class_name(gui_c_path)
    required_desktop_fields = [
        "cat > AppDir/misrc.desktop <<EOF",
        "Exec=misrc_gui",
        "Icon=misrc",
        f"StartupWMClass={wm_class}",
        f"X-GNOME-WMClass={wm_class}",
        "Terminal=false",
        "StartupNotify=true",
        "ln -sf misrc.png AppDir/.DirIcon",
    ]
    for field in required_desktop_fields:
        if field not in workflow_text:
            return fail(f"Missing Linux desktop integration field in workflow/AppRun: {field}")
    return 0


def check_macos_layout_policy(gui_c_path: Path) -> int:
    source = read_text(gui_c_path)
    width_body = extract_function_body(source, "static int gui_layout_width(void)")
    height_body = extract_function_body(source, "static int gui_layout_height(void)")

    if "#if defined(__APPLE__)" not in width_body:
        return fail("gui_layout_width() is missing __APPLE__ guard")
    if "#if defined(__APPLE__)" not in height_body:
        return fail("gui_layout_height() is missing __APPLE__ guard")
    if "GetScreenWidth();" not in width_body:
        return fail("gui_layout_width() must use GetScreenWidth() on macOS")
    if "GetScreenHeight();" not in height_body:
        return fail("gui_layout_height() must use GetScreenHeight() on macOS")
    if "GetRenderWidth();" not in width_body:
        return fail("gui_layout_width() must use GetRenderWidth() for non-macOS")
    if "GetRenderHeight();" not in height_body:
        return fail("gui_layout_height() must use GetRenderHeight() for non-macOS")
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


def check_apprun_static_contract(workflow_path: Path, gui_c_path: Path) -> int:
    workflow_text = read_text(workflow_path)
    apprun = extract_apprun_script(workflow_text)
    wm_class = read_gui_window_class_name(gui_c_path)
    required_snippets = [
        "install_shortcuts()",
        "--create-shortcut",
        "local stable_appimage=\"$local_bin_dir/misrc_gui.AppImage\"",
        "ln -sfn \"$appimage_path\" \"$stable_appimage\"",
        f"local startup_wm_class=\"{wm_class}\"",
        "Icon=misrc",
        "StartupWMClass=${escaped_startup_wm_class}",
        "X-GNOME-WMClass=${escaped_startup_wm_class}",
        "StartupNotify=true",
    ]
    for snippet in required_snippets:
        if snippet not in apprun:
            return fail(f"AppRun shortcut contract is missing snippet: {snippet}")
    if "Exec=\\\"${escaped_launcher_exec_path}\\\" %U" not in apprun and "Exec=\\\\\\\"${escaped_launcher_exec_path}\\\\\\\" %U" not in apprun:
        return fail("AppRun shortcut contract is missing expected Exec launcher format")
    return 0


def check_apprun_runtime_behavior(workflow_path: Path, icon_path: Path, gui_c_path: Path) -> int:
    if not sys.platform.startswith("linux"):
        print("SKIP: AppRun runtime behavior (linux-only)")
        return 0
    if shutil.which("bash") is None:
        print("SKIP: AppRun runtime behavior (bash not available)")
        return 0

    workflow_text = read_text(workflow_path)
    script = extract_apprun_script(workflow_text)

    with tempfile.TemporaryDirectory(prefix="misrc_ci_guard_") as temp_root:
        root = Path(temp_root)
        appdir = root / "AppDir"
        (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)

        apprun_path = appdir / "AppRun"
        apprun_path.write_text(script, encoding="utf-8")
        apprun_path.chmod(apprun_path.stat().st_mode | stat.S_IXUSR)

        run_checked(["bash", "-n", str(apprun_path)])

        for exe in ("misrc_gui", "misrc_capture", "misrc_extract"):
            exe_path = appdir / "usr/bin" / exe
            if exe == "misrc_gui":
                exe_path.write_text(
                    "#!/usr/bin/env bash\n"
                    "if [[ \"${1:-}\" == \"--version\" ]]; then\n"
                    "  echo \"test-version\"\n"
                    "  exit 0\n"
                    "fi\n"
                    "exit 0\n",
                    encoding="utf-8",
                )
            else:
                exe_path.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
            exe_path.chmod(exe_path.stat().st_mode | stat.S_IXUSR)

        if icon_path.exists():
            shutil.copy2(icon_path, appdir / "misrc.png")
        else:
            (appdir / "misrc.png").write_bytes(b"\x89PNG\r\n\x1a\n")

        appimage_path = root / "MISRC Test Build.AppImage"
        appimage_path.write_text("fake", encoding="utf-8")
        appimage_path.chmod(appimage_path.stat().st_mode | stat.S_IXUSR)

        home = root / "home"
        desktop_dir = home / "Desktop"
        desktop_dir.mkdir(parents=True, exist_ok=True)

        env = os.environ.copy()
        env["HOME"] = str(home)
        env["APPIMAGE"] = str(appimage_path)
        env.pop("XDG_DATA_HOME", None)

        run_checked(["bash", str(apprun_path), "--create-shortcut"], env=env)

        launcher_path = home / ".local/share/applications/misrc_gui.desktop"
        desktop_shortcut_path = home / "Desktop/MISRC GUI.desktop"
        icon_install_path = home / ".local/share/icons/hicolor/512x512/apps/misrc.png"
        stable_exec_path = home / ".local/bin/misrc_gui.AppImage"

        if not launcher_path.exists():
            return fail("AppRun --create-shortcut did not create launcher file")
        if not desktop_shortcut_path.exists():
            return fail("AppRun --create-shortcut did not create Desktop shortcut")
        if not icon_install_path.exists():
            return fail("AppRun --create-shortcut did not install icon")
        if not stable_exec_path.exists():
            return fail("AppRun --create-shortcut did not create stable AppImage launcher path")
        if not os.path.samefile(stable_exec_path, appimage_path):
            return fail("Stable AppImage launcher path does not resolve to current AppImage")

        launcher = read_text(launcher_path)
        expected_exec = f'Exec=\"{stable_exec_path}\" %U'
        if expected_exec not in launcher:
            return fail(f"Launcher Exec entry mismatch. Expected: {expected_exec}")
        expected_wm_class = read_gui_window_class_name(gui_c_path)
        for required in ("Icon=misrc", "Terminal=false", f"StartupWMClass={expected_wm_class}", f"X-GNOME-WMClass={expected_wm_class}", "StartupNotify=true"):
            if required not in launcher:
                return fail(f"Launcher is missing required key: {required}")

        run_checked(["bash", str(apprun_path), "--smoke-test"], env=env)

    return 0


def check_wm_class_consistency(gui_c_path: Path, repo_root: Path) -> int:
    """WM_CLASS is what ties a running window back to its launcher. misrc_gui.c
    creates the window under GUI_WINDOW_CLASS_NAME, so every .desktop we write --
    release AppImage, local AppImage, and the self-hosted deploy that installs onto
    workflow-master -- must set StartupWMClass to exactly that string, and must
    never decorate it with a build version, or a launcher written by one build
    stops matching the next build's window and the dock loses the app icon."""
    wm_class = read_gui_window_class_name(gui_c_path)
    workflow_path = repo_root / ".github/workflows/build.yml"
    deploy_workflow_path = repo_root / ".github/workflows/selfhosted-deploy.yml"
    local_appimage_script = repo_root / "scripts/build-appimage-local.sh"
    required_by_surface = {
        workflow_path: (
            f"StartupWMClass={wm_class}",
            f"X-GNOME-WMClass={wm_class}",
            f'local startup_wm_class="{wm_class}"',
        ),
        local_appimage_script: (
            f"StartupWMClass={wm_class}",
            f"X-GNOME-WMClass={wm_class}",
        ),
    }
    for path, required in required_by_surface.items():
        if not path.exists():
            return fail(f"WM_CLASS surface is missing: {path}")
        text = read_text(path)
        for snippet in required:
            if snippet not in text:
                return fail(
                    f"{path.name} does not match GUI_WINDOW_CLASS_NAME "
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
                f'GUI_WINDOW_CLASS_NAME ("{wm_class}") in misrc_gui.c'
            )
    offenders = find_versioned_wm_class(entry)
    if offenders:
        return fail(
            f"selfhosted-deploy.yml bakes a build version into WM_CLASS: {offenders}"
        )
    # RESOURCE_NAME was the pre-GUI_WINDOW_CLASS_NAME shim. It pins only the instance
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
        built = subprocess.run(compile_cmd, capture_output=True, text=True)
        if built.returncode != 0:
            return fail(f"mediamtx config harness failed to compile:\n{built.stderr.strip()}")
        ran = subprocess.run([str(exe_path)], capture_output=True, text=True)
        if ran.returncode != 0:
            return fail(
                "mediamtx config harness failed:\n"
                f"{ran.stdout.strip()}\n{ran.stderr.strip()}"
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
    release proves only that the transfer worked. Both AppImage paths -- CI and
    scripts/build-appimage-local.sh -- must go through the one script, or a
    locally built AppImage ends up carrying a different server."""
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
        (repo_root / ".github/workflows/build.yml", "the release AppImage"),
        (repo_root / "scripts/build-appimage-local.sh", "the local AppImage"),
    ):
        if "fetch-mediamtx.sh" not in read_text(consumer):
            return fail(f"{label} does not stage mediamtx via scripts/fetch-mediamtx.sh")
    return 0


def check_rtsp_settings_roundtrip(repo_root: Path) -> int:
    """gui_settings.c keeps defaults, the writer and the parser in three
    separate places. A field added to two of them is silent: the setting
    appears to work until it is reloaded, and then it is quietly the default
    again. Assert every stream setting exists in all three."""
    struct_src = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_app.h")
    settings_src = read_text(repo_root / "misrc_tools/misrc_gui/core/gui_settings.c")

    fields = sorted(set(
        re.findall(r"\b(rtsp_[a-z0-9_]+|mediamtx_path)\b(?:\[\d+\])?\s*;", struct_src)
    ))
    if not fields:
        return fail("gui_app.h declares no rtsp_* stream settings")

    for field in fields:
        # The three sites, checked as the three distinct shapes they take.
        # The writer escapes its quotes, so its key looks like \\"field\\" in source.
        defaulted = re.search(rf"settings->{field}(\[0\])?\s*=", settings_src) is not None
        written = f'\\"{field}\\"' in settings_src
        parsed = f'find_value(content, "{field}")' in settings_src

        missing = [name for name, ok in
                   (("a default", defaulted), ("a writer line", written),
                    ("a find_value() parse", parsed)) if not ok]
        if missing:
            return fail(
                f"gui_settings.c is missing {' and '.join(missing)} for {field}. "
                "A setting present in only some of the three sites is silent: it "
                "appears to work until it is reloaded as the default."
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
        "$ZipPath = \"Windows_MISRC_${{ steps.version.outputs.version }}_x86.zip\"",
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
    <Platform>_MISRC_<version>_<arch>.<ext> with the platform name capitalized
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
        "APPIMAGE_NAME=\"Linux_MISRC_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.AppImage\"",
        "ZIP_NAME=\"Linux_MISRC_${BUILD_VERSION}_${{ matrix.artifact_suffix }}.zip\"",
        "path: Linux_MISRC_*_${{ matrix.artifact_suffix }}.zip",
        "$ZipPath = \"Windows_MISRC_${{ steps.version.outputs.version }}_x86.zip\"",
        "path: Windows_MISRC_*_x86.zip",
        "$ZipPath = \"Windows_MISRC_${{ steps.version.outputs.version }}_arm64.zip\"",
        "path: Windows_MISRC_*_arm64.zip",
        "DMG_NAME=\"macOS_MISRC_${BUILD_VERSION}_universal.dmg\"",
        "path: macOS_MISRC_*_universal.dmg",
        "release-assets/**/Linux_MISRC_*_x86.zip",
        "release-assets/**/Linux_MISRC_*_arm64.zip",
        "release-assets/**/Windows_MISRC_*_x86.zip",
        "release-assets/**/Windows_MISRC_*_arm64.zip",
        "release-assets/**/macOS_MISRC_*_universal.dmg",
        "APK_ARM64=\"Android_MISRC_${TAG}_arm64.apk\"",
        "release-assets/**/Android_MISRC_*_arm64.apk",
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
    if "Android_MISRC_${VERSION}_arm64.apk" not in apk_text:
        return fail(
            "android/build-apk.sh must name the APK Android_MISRC_${VERSION}_arm64.apk "
            "(capitalized platform prefix, matching the release convention)."
        )
    if "android_MISRC_${VERSION}_arm64.apk" in apk_text:
        return fail(
            "android/build-apk.sh still uses the lowercase android_MISRC_ prefix "
            "(v1.1.7 malform); use Android_MISRC_."
        )
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
    if "*_MISRC_dev-*" not in wf:
        return fail("Release pre-upload assertion must match '*_MISRC_dev-*' dev-named artifacts.")
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
        ("meson FFT policy", lambda: check_meson_fft_policy(meson_path)),
        ("meson vendored hsdaoh policy", lambda: check_meson_vendored_hsdaoh_policy(meson_path)),
        ("meson FX3 native-build policy", lambda: check_meson_fx3_policy(meson_path)),
        ("cross-platform smoke tests", lambda: check_cross_platform_smoke_tests(workflow_path)),
        ("linux desktop metadata", lambda: check_linux_desktop_metadata(workflow_path, gui_c_path)),
        ("WM_CLASS matches GUI window class", lambda: check_wm_class_consistency(gui_c_path, repo_root)),
        ("macOS layout policy", lambda: check_macos_layout_policy(gui_c_path)),
        ("macOS startup admin elevation contract", lambda: check_macos_admin_elevation_contract(gui_c_path)),
        ("Windows meson subsystem contract", lambda: check_windows_meson_subsystem_contract(meson_path)),
        ("dev version naming", lambda: check_dev_version_naming(repo_root, meson_path, workflow_path)),
        ("no tracked generated dirty sources", lambda: check_no_tracked_generated_dirty_sources(repo_root)),
        ("Windows GUI link no DLL import libs", lambda: check_windows_gui_link_no_dll_import_libs(meson_path)),
        ("optional-dep guard consistency", lambda: check_optional_dep_guard_consistency(repo_root)),
        ("debug-view runtime contract", lambda: check_debug_view_contract(gui_c_path)),
        ("settings persistence contract", lambda: check_settings_persistence_contract(gui_settings_c_path)),
        ("FLAC large-file offsets contract", lambda: check_flac_large_file_offsets_contract(flac_writer_c_path)),
        ("preview tap single-slot contract", lambda: check_preview_tap_single_slot_contract(repo_root)),
        ("alsa never stores a card index", lambda: check_alsa_never_stores_a_card_index(repo_root)),
        ("bundled mediamtx contract", lambda: check_bundled_mediamtx_contract(repo_root)),
        ("rtsp settings round-trip", lambda: check_rtsp_settings_roundtrip(repo_root)),
        ("AppRun static contract", lambda: check_apprun_static_contract(workflow_path, gui_c_path)),
        ("Windows packaging assertions", lambda: check_windows_packaging_assertions(workflow_path)),
        ("Android packaging assertions", lambda: check_android_packaging_assertions(workflow_path)),
        ("release artifact naming contract", lambda: check_release_artifact_naming_contract(repo_root, workflow_path)),
        ("release version resolution contract", lambda: check_release_version_resolution_contract(repo_root, workflow_path)),
        ("build workflow entrypoint contract", lambda: check_build_workflow_entrypoint_contract(workflow_path)),
        ("legacy release-sanity workflow removed", lambda: check_no_legacy_release_sanity_workflow(legacy_workflow_path)),
        ("no capture-stability Actions clutter", lambda: check_no_capture_stability_clutter(workflow_path)),
        ("local build bootstrap contract", lambda: check_local_build_bootstrap_contract(repo_root, dev_notes_path, installation_md_path)),
        ("local deps cache contract", lambda: check_local_deps_cache_contract(repo_root, workflow_path, dev_notes_path, installation_md_path)),
    ]
    if not args.static_only:
        checks.insert(7, ("AppRun runtime behavior", lambda: check_apprun_runtime_behavior(workflow_path, icon_path, gui_c_path)))
        checks.insert(8, ("record ringbuffer fallback runtime", lambda: check_record_ringbuffer_fallback_runtime(repo_root)))
        checks.insert(9, ("FLAC STREAMINFO total_samples runtime", lambda: check_flac_streaminfo_total_samples_runtime(repo_root)))
        checks.insert(10, ("preview tap mux runtime", lambda: check_preview_tap_mux_runtime(repo_root)))
        checks.insert(11, ("mediamtx config runtime", lambda: check_mediamtx_config_runtime(repo_root)))
        checks.insert(12, ("alsa device resolution", lambda: check_alsa_device_resolution(repo_root)))
        checks.insert(10, ("built GUI links vendored hsdaoh", lambda: check_built_gui_links_vendored_hsdaoh(repo_root, args.gui_path)))
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
