#!/usr/bin/env python3
"""Release download link check: verify the manifest-driven update pipeline.

The GUI updater (misrc_tools/misrc_gui/core/gui_updater.c + ui/gui_ui.c) is
manifest-driven since the 2026-10-22 rework:

- The manual "Releases Page" button opens the releases PAGE - a page URL can
  never 404 per platform the way a compiled-in per-platform asset URL could.
- Every installed app fetches update_manifest.json from a FIXED raw URL on
  main (the URL is compiled into the app; the manifest CONTENTS are not). The
  manifest defines the releases page, the tag lookup URL, the download base
  and the six per-platform asset filename patterns ({tag} placeholders), so a
  naming/URL fix for every installed binary is a commit to main - no new
  release required (the "auto patch").
- Confirmed self-update (two-click) downloads the manifest-named asset and
  installs it with the old binary archived by rename (never deleted).

Regressions this prevents (v1.2.4 class, recurred 2026-10-22): the old
updater built asset URLs from filename patterns compiled INTO the binary;
whenever that mapping drifted from the published assets (v1.2.4 shipped
patterns missing the "_GUI" infix), every platform's download 404'd and
installed binaries could never be fixed retroactively. This test checks the
pipeline END TO END the way the app actually behaves:

- extracts the manifest URL + compiled fallback URLs from the app source
  (source of truth: the strings actually compiled into the app),
- fetches the LIVE raw-main manifest exactly as the app does and validates
  its structure (schema, all 6 platform patterns with {tag}, https URLs),
- HEAD-checks the live manifest's releases page URL,
- resolves the latest tag via the manifest's tag_lookup_url (releases/latest
  redirect -> Location, same as the app), and
- HEAD-checks all six manifest-built asset URLs - the exact URLs a confirmed
  self-update would download,
- plus the REPO's update_manifest.json against the same latest published
  release, so a broken manifest is caught BEFORE merge (a PR run sees the
  repo manifest while raw main still serves the previous one).

--tag vX.Y.Z (release job post-publish step) checks the repo manifest - the
commit being released - against the just-published tag, so a manifest/asset
mismatch cannot ship with a release. Live-manifest equality is not asserted
there (the raw CDN can lag minutes behind the push).
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GUI_UI_C = REPO_ROOT / "misrc_tools" / "misrc_gui" / "ui" / "gui_ui.c"
GUI_UPDATER_H = REPO_ROOT / "misrc_tools" / "misrc_gui" / "core" / "gui_updater.h"
REPO_MANIFEST = REPO_ROOT / "update_manifest.json"
CURL_TIMEOUT_S = 30
MIN_PATTERN_COUNT = 6

REQUIRED_KEYS = [
    "releases_page",
    "tag_lookup_url",
    "download_base_url",
    "asset_android",
    "asset_macos",
    "asset_windows_x86",
    "asset_windows_arm64",
    "asset_linux_x86",
    "asset_linux_arm64",
]
ASSET_PATTERN_KEYS = REQUIRED_KEYS[3:]
TAG_FROM_URL_RE = re.compile(r"/releases/tag/([^/?#]+)")
MANIFEST_URL_RE = re.compile(r'"(https://raw\.githubusercontent\.com/[^"]+)"')


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def run_curl(args, timeout=CURL_TIMEOUT_S + 10):
    try:
        return subprocess.run(
            args, capture_output=True, text=True, timeout=timeout, check=False
        )
    except FileNotFoundError:
        return None
    except subprocess.TimeoutExpired:
        return None


def head_check_url(url: str) -> tuple:
    """HEAD-check a URL following redirects. Returns (ok, detail)."""
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


def extract_manifest_url_from_app() -> str:
    """The update-manifest raw URL exactly as compiled into the app.

    The URL macro lives in gui_updater.h (the app pipeline's source of
    truth); gui_ui.c must include that header for the pipeline to exist."""
    if not GUI_UI_C.exists():
        raise SystemExit(fail(f"gui_ui.c not found: {GUI_UI_C}"))
    if '#include "../core/gui_updater.h"' not in GUI_UI_C.read_text(encoding="utf-8"):
        raise SystemExit(
            fail("gui_ui.c does not include core/gui_updater.h - the manifest-driven "
                 "update pipeline wiring is missing")
        )
    if not GUI_UPDATER_H.exists():
        raise SystemExit(fail(f"gui_updater.h not found: {GUI_UPDATER_H}"))
    for match in MANIFEST_URL_RE.finditer(GUI_UPDATER_H.read_text(encoding="utf-8")):
        url = match.group(1)
        if url.endswith("/update_manifest.json"):
            return url
    raise SystemExit(
        fail("no https://raw.githubusercontent.com/.../update_manifest.json URL found "
             "in gui_updater.h - the app has no manifest URL to fetch")
    )


def validate_manifest(manifest, label: str) -> None:
    """Structural contract of the auto-patch manifest (mirrors the app's
    gui_updater_parse_manifest_text acceptance rules)."""
    if not isinstance(manifest, dict):
        raise SystemExit(fail(f"{label}: manifest is not a JSON object"))
    if manifest.get("schema") != 1:
        raise SystemExit(
            fail(f"{label}: schema must be 1 (got {manifest.get('schema')!r})")
        )
    for key in REQUIRED_KEYS:
        value = manifest.get(key)
        if not isinstance(value, str) or not value:
            raise SystemExit(fail(f"{label}: missing or empty key: {key}"))
    for key in ("releases_page", "tag_lookup_url", "download_base_url"):
        if not manifest[key].startswith("https://"):
            raise SystemExit(
                fail(f"{label}: {key} must be an https URL: {manifest[key]}")
            )
    for key in ASSET_PATTERN_KEYS:
        pattern = manifest[key]
        if "{tag}" not in pattern:
            raise SystemExit(
                fail(f"{label}: asset pattern must contain the {{tag}} placeholder: "
                     f"{key}={pattern}")
            )
        if any(ch in pattern for ch in ('"', "'", "\\", " ", "\n", "\r")):
            raise SystemExit(
                fail(f"{label}: asset pattern must be a single-line filename-safe "
                     f"string: {key}={pattern!r}")
            )


def load_repo_manifest():
    if not REPO_MANIFEST.exists():
        raise SystemExit(
            fail(f"repo update_manifest.json not found: {REPO_MANIFEST} - the auto-patch "
                 "source must be committed at the repo root (served to every "
                 "installed app via the fixed raw URL on main)")
        )
    try:
        manifest = json.loads(REPO_MANIFEST.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(fail(f"repo update_manifest.json is not valid JSON: {exc}"))
    validate_manifest(manifest, "repo manifest")
    return manifest


def fetch_live_manifest(manifest_url: str, attempts: int = 3):
    """Fetch the live manifest exactly the way the app does (curl -fsSL).

    Retries spread over ~30s: the raw CDN can lag minutes behind a push to
    main, so a run started right after a push (or the very first push that
    introduces the manifest) can briefly 404 until the CDN catches up."""
    import time

    last_detail = ""
    for attempt in range(1, attempts + 1):
        proc = run_curl(["curl", "-fsSL", "--max-time", "20", manifest_url])
        if proc is not None and proc.returncode == 0:
            try:
                manifest = json.loads(proc.stdout)
            except json.JSONDecodeError as exc:
                raise SystemExit(
                    fail(f"live manifest is not valid JSON ({manifest_url}): {exc}")
                )
            validate_manifest(manifest, "live manifest")
            return manifest
        if proc is not None:
            last_detail = (proc.stderr or "").strip()[:160]
        if attempt < attempts:
            print(f"live manifest fetch attempt {attempt}/{attempts} failed "
                  f"({last_detail or 'curl unavailable/timeout'}); retrying in 15s")
            time.sleep(15)
    raise SystemExit(
        fail(f"live manifest fetch failed ({manifest_url}) after {attempts} attempts: "
             f"{last_detail or 'curl unavailable/timeout'} - if this run started right "
             "after the push that introduced update_manifest.json, the raw CDN is "
             "still catching up; re-run the job in a few minutes")
    )


def resolve_latest_tag(tag_lookup_url: str) -> tuple:
    """Resolve the latest release tag exactly the way the app does: HEAD the
    manifest's tag_lookup_url and parse the tag out of the Location redirect."""
    proc = run_curl(["curl", "-fsSI", "--max-time", "20", tag_lookup_url])
    if proc is None or proc.returncode != 0:
        detail = ""
        if proc is not None:
            detail = (proc.stderr or "").strip()[:120]
        return False, "", f"tag lookup request failed ({tag_lookup_url}): {detail or 'curl unavailable/timeout'}"
    location = ""
    for line in proc.stdout.splitlines():
        if line.lower().startswith("location:"):
            location = line.split(":", 1)[1].strip()
            break
    if not location:
        return False, "", "releases/latest redirect Location header missing"
    match = TAG_FROM_URL_RE.search(location)
    if not match:
        return False, "", f"cannot parse tag from redirect: {location}"
    return True, match.group(1), ""


def check_manifest_urls_for_tag(manifest, tag: str, label: str) -> int:
    """HEAD-check every asset URL the manifest builds for this tag - the exact
    URLs a confirmed self-update on each platform would download."""
    failures = 0
    for key in ASSET_PATTERN_KEYS:
        asset = manifest[key].replace("{tag}", tag)
        url = f"{manifest['download_base_url']}/{tag}/{asset}"
        ok, detail = head_check_url(url)
        status = "OK  " if ok else "FAIL"
        print(f"{status} [{label}] {key}: {url} [{detail}]")
        if not ok:
            failures += 1
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the manifest-driven release download pipeline"
    )
    parser.add_argument(
        "--tag",
        default=None,
        help="Check the repo manifest against this tag instead of the latest "
             "published release (used by the release job right after "
             "publishing; the raw CDN can lag the push, so live equality is "
             "not asserted in this mode).",
    )
    args = parser.parse_args()

    manifest_url = extract_manifest_url_from_app()
    repo_manifest = load_repo_manifest()

    if args.tag:
        tag = args.tag
        print(f"Checking tag (override): {tag}")
        print(f"Repo manifest: {REPO_MANIFEST}")
        failures = check_manifest_urls_for_tag(repo_manifest, tag, "repo manifest")
        if failures:
            return fail(
                f"{failures}/{MIN_PATTERN_COUNT} repo-manifest asset URLs do not "
                f"resolve for tag {tag} - the manifest being released does not "
                "match the published asset names; every installed app's "
                "confirmed self-update would download a 404. Fix "
                "update_manifest.json (or the release asset naming) in THIS "
                "commit."
            )
        print(f"All {MIN_PATTERN_COUNT} repo-manifest asset URLs resolve for tag {tag}.")
        return 0

    # Live manifest: exactly what every installed app pulls on update check.
    live_manifest = fetch_live_manifest(manifest_url)
    print(f"Live manifest fetched and valid: {manifest_url}")

    ok, detail = head_check_url(live_manifest["releases_page"])
    print(f"{'OK  ' if ok else 'FAIL'} [live manifest] releases page: "
          f"{live_manifest['releases_page']} [{detail}]")
    if not ok:
        return fail("the live manifest's releases page URL does not resolve - the "
                    "manual download path is broken for every installed app")

    ok, tag, err = resolve_latest_tag(live_manifest["tag_lookup_url"])
    if not ok:
        return fail(err)
    print(f"Latest published release (via manifest tag_lookup_url): {tag}")

    failures = check_manifest_urls_for_tag(live_manifest, tag, "live manifest")
    if failures:
        return fail(
            f"{failures}/{MIN_PATTERN_COUNT} live-manifest asset URLs do not resolve "
            f"for tag {tag} - every installed app's confirmed self-update would "
            "download a 404 RIGHT NOW. Fix = edit update_manifest.json on main "
            "(the auto patch); no release needed."
        )

    failures = check_manifest_urls_for_tag(repo_manifest, tag, "repo manifest")
    if failures:
        return fail(
            f"{failures}/{MIN_PATTERN_COUNT} repo-manifest asset URLs do not resolve "
            f"for tag {tag} - the manifest in THIS commit is broken against the "
            "latest published release (would break apps once merged and the raw "
            "CDN catches up)."
        )

    if live_manifest != repo_manifest:
        print("NOTE: live raw-main manifest differs from the repo manifest "
             "(raw CDN cache lag, or a pending unmerged change).")

    print(f"All {MIN_PATTERN_COUNT} manifest asset URLs + the releases page resolve "
          f"for tag {tag} (live manifest + repo manifest).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
