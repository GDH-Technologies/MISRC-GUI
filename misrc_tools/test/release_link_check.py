#!/usr/bin/env python3
"""Release download link check: verify the exact URLs the in-app updater builds.

The GUI update checker (misrc_tools/misrc_gui/ui/gui_ui.c) resolves the latest
release tag via the releases/latest redirect (Location header) and builds a
per-platform asset URL from a filename pattern compiled into the app
(gui_ui_build_release_asset_filename_for_platform -> <Platform>_MISRC_GUI_
<tag>_<arch>.<ext>). The CI workflow publishes assets with exactly that naming.

Regression this prevents (user report against v1.2.4): gui_ui.c shipped
patterns missing the "_GUI" infix (Windows_MISRC_%s_x86.zip) while the workflow
publishes Windows_MISRC_GUI_<tag>_x86.zip, so every platform's update-check
Download button 404'd for every release. The workflow-side naming is pinned by
check_release_artifact_naming_contract, but nothing cross-checked the app-side
mapping. This test closes that gap END TO END: it extracts the filename
patterns from the app source (the source of truth: the strings actually
compiled into the app), resolves the tag exactly the way the app does, and
HEAD-checks every download URL.

Runs in CI:
- the `release-link-check` workflow job, on every trigger, against the latest
  PUBLISHED release (catches app-mapping drift or mis-named assets), and
- the release job's post-publish step with --tag <released tag> (catches a
  broken mapping in the same run that publishes, before anyone clicks
  Download).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GUI_UI_C = REPO_ROOT / "misrc_tools" / "misrc_gui" / "ui" / "gui_ui.c"
RELEASES_LATEST_URL = "https://github.com/harrypm/MISRC-GUI/releases/latest"
RELEASES_DOWNLOAD_BASE_URL = "https://github.com/harrypm/MISRC-GUI/releases/download"
CURL_TIMEOUT_S = 30
# Windows x86/arm64, Linux x86/arm64, macOS universal, Android arm64.
MIN_PATTERN_COUNT = 6

# Asset filename patterns as assigned in gui_ui.c's
# gui_ui_build_release_asset_filename_for_platform (the exact strings compiled
# into the app). Extraction is naming-neutral (with or without _GUI) so the
# live URL check is what catches a wrong name.
PATTERN_RE = re.compile(
    r'"((?:Android|macOS|Windows|Linux)_MISRC[^"]*%s[^"]*\.(?:apk|dmg|zip))"'
)
TAG_FROM_URL_RE = re.compile(r"/releases/tag/([^/?#]+)")


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def extract_patterns_from_app() -> list:
    if not GUI_UI_C.exists():
        raise SystemExit(fail(f"gui_ui.c not found: {GUI_UI_C}"))
    text = GUI_UI_C.read_text(encoding="utf-8")
    patterns = PATTERN_RE.findall(text)
    # De-duplicate while keeping order (nested #if branches can repeat names).
    seen = set()
    unique = []
    for p in patterns:
        if p not in seen:
            seen.add(p)
            unique.append(p)
    return unique


def run_curl(args, timeout=CURL_TIMEOUT_S + 10):
    try:
        return subprocess.run(
            args, capture_output=True, text=True, timeout=timeout, check=False
        )
    except FileNotFoundError:
        return None
    except subprocess.TimeoutExpired:
        return None


def resolve_latest_tag() -> tuple:
    """Resolve the latest release tag exactly the way the app does: HEAD the
    releases/latest URL and parse the tag out of the Location redirect."""
    proc = run_curl(["curl", "-fsSI", "--max-time", "20", RELEASES_LATEST_URL])
    if proc is None or proc.returncode != 0:
        detail = ""
        if proc is not None:
            detail = (proc.stderr or "").strip()[:120]
        return False, "", f"releases/latest request failed: {detail or 'curl unavailable/timeout'}"
    location = ""
    for line in proc.stdout.splitlines():
        lowered = line.lower()
        if lowered.startswith("location:"):
            location = line.split(":", 1)[1].strip()
            break
    if not location:
        return False, "", "releases/latest redirect Location header missing"
    m = TAG_FROM_URL_RE.search(location)
    if not m:
        return False, "", f"cannot parse tag from redirect: {location}"
    return True, m.group(1), ""


def head_check_url(url: str) -> tuple:
    """HEAD-check a download URL following redirects. Returns (ok, detail)."""
    proc = run_curl([
        "curl", "-sSIL", "--max-time", str(CURL_TIMEOUT_S),
        "-o", "/dev/null", "-w", "%{http_code}", url,
    ])
    if proc is None:
        return False, "curl unavailable or timed out"
    code = (proc.stdout or "").strip()
    if proc.returncode != 0:
        err = (proc.stderr or "").strip()[:160]
        return False, f"HTTP {code or '?'} (curl rc={proc.returncode}) {err}"
    return code == "200", f"HTTP {code}"


def check_urls_for_tag(tag: str, patterns: list) -> int:
    failures = 0
    for pattern in patterns:
        asset = pattern.replace("%s", tag)
        url = f"{RELEASES_DOWNLOAD_BASE_URL}/{tag}/{asset}"
        ok, detail = head_check_url(url)
        status = "OK  " if ok else "FAIL"
        print(f"{status} {url} [{detail}]")
        if not ok:
            failures += 1
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the in-app updater's release download URLs resolve"
    )
    parser.add_argument(
        "--tag",
        default=None,
        help="Check this tag instead of the latest published release "
        "(used by the release job right after publishing).",
    )
    args = parser.parse_args()

    patterns = extract_patterns_from_app()
    if len(patterns) < MIN_PATTERN_COUNT:
        return fail(
            f"expected at least {MIN_PATTERN_COUNT} asset filename patterns in "
            f"gui_ui.c, extracted {len(patterns)}: {patterns} — the updater "
            "mapping extraction is broken or the platform set changed."
        )

    if args.tag:
        tag = args.tag
        print(f"Checking tag (override): {tag}")
    else:
        ok, tag, err = resolve_latest_tag()
        if not ok:
            return fail(err)
        print(f"Latest published release (releases/latest redirect): {tag}")

    print(f"Checking {len(patterns)} app-asset URL(s) for tag {tag}:")
    failures = check_urls_for_tag(tag, patterns)
    if failures:
        return fail(
            f"{failures}/{len(patterns)} release download URL(s) the in-app "
            f"updater builds do not resolve for tag {tag} — the app's asset "
            "filename mapping does not match the published release asset "
            "names (see gui_ui.c gui_ui_build_release_asset_filename_for_platform "
            "vs the workflow asset naming)."
        )
    print(f"All {len(patterns)} app-asset URLs resolve for tag {tag}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
