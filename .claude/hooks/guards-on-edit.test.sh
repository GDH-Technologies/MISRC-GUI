#!/usr/bin/env bash
# Regression suite for guards-on-edit.py.
#
# Drives the hook the way Claude Code does (PostToolUse JSON on stdin) and asserts: an
# unmatched path or a bad payload exits 0 without running the suite; MISRC_GUARDS_ON_EDIT=0
# short-circuits; every guard-matched path pattern is recognised (against a fake checkout
# whose suite records that it ran); a matched path runs the real suite on this checkout; and
# a failing suite exits 2 with the guard names on stderr.
#
# Run from any checkout:   bash .claude/hooks/guards-on-edit.test.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HOOK="$HERE/guards-on-edit.py"
ROOT="$(cd -- "$HERE/../.." && pwd)"
ERR="$(mktemp)"
TMP="$(mktemp -d)"
trap 'rm -rf "$ERR" "$TMP"' EXIT
PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$*"; }
ms()  { date +%s%3N; }
run() { # file_path [cwd]
  printf '{"hook_event_name":"PostToolUse","tool_name":"Edit","cwd":"%s","tool_input":{"file_path":"%s"}}' \
    "${2:-$ROOT}" "$1" | python3 "$HOOK" 2>"$ERR"
}

echo "hook: $HOOK"
echo "root: $ROOT"
echo

# --- 1. unmatched source file exits 0 without running the suite -------------------------
t0=$(ms); set +e; run "$ROOT/misrc_tools/misrc_gui/ui/gui_ui.c"; rc=$?; set -e; dt=$(( $(ms) - t0 ))
if [ "$rc" -eq 0 ] && [ "$dt" -lt 1000 ]; then ok "unmatched .c exits 0 without running the suite (${dt} ms)"; else bad "unmatched rc=$rc in ${dt} ms"; fi

# --- 2. bad payloads never block ----------------------------------------------------------
set +e; printf 'not json' | python3 "$HOOK" 2>/dev/null; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "garbage payload exits 0"; else bad "garbage payload rc=$rc"; fi
set +e; printf '{}' | python3 "$HOOK" 2>/dev/null; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "empty payload exits 0"; else bad "empty payload rc=$rc"; fi

# --- 3. disabled by environment ------------------------------------------------------------
set +e; MISRC_GUARDS_ON_EDIT=0 run "$ROOT/misrc_tools/meson.build"; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "MISRC_GUARDS_ON_EDIT=0 short-circuits"; else bad "disable rc=$rc"; fi

# --- 4. every guard-matched pattern is recognised (fake checkout records a run) ------------
FAKE="$TMP/ok"
mkdir -p "$FAKE/misrc_tools/test"
printf 'import pathlib, sys\npathlib.Path(sys.argv[0]).with_name("RAN").touch()\nsys.exit(0)\n' > "$FAKE/misrc_tools/test/ci_guard_tests.py"
for p in .github/workflows/selfhosted-deploy.yml misrc_tools/meson.build misrc_tools/meson_options.txt \
         scripts/build-local.sh scripts/build-appimage-local.sh scripts/build-deps-unix.sh scripts/build-local.ps1 \
         scripts/fetch-mediamtx.sh scripts/publish-deps-cache.sh misrc_tools/git-version.sh \
         misrc_tools/test/ci_guard_tests.py misrc_tools/test/gui_stream_soak_harness.c; do
  rm -f "$FAKE/misrc_tools/test/RAN"
  set +e; run "$FAKE/$p" "$FAKE"; rc=$?; set -e
  if [ "$rc" -eq 0 ] && [ -f "$FAKE/misrc_tools/test/RAN" ]; then ok "matches $p"; else bad "did not run for $p (rc=$rc)"; fi
done
for p in misrc_tools/misrc_gui/input/gui_capture.c README.md docs/gdh-selfhosted-ci.md .github/workflows/build.yml; do
  rm -f "$FAKE/misrc_tools/test/RAN"
  set +e; run "$FAKE/$p" "$FAKE"; rc=$?; set -e
  if [ "$rc" -eq 0 ] && [ ! -f "$FAKE/misrc_tools/test/RAN" ]; then ok "ignores $p"; else bad "ran for $p (rc=$rc)"; fi
done

# --- 5. a matched path runs the real suite on this checkout ---------------------------------
t0=$(ms); set +e; run "$ROOT/misrc_tools/meson.build"; rc=$?; set -e; dt=$(( $(ms) - t0 ))
if [ "$rc" -eq 0 ] && [ "$dt" -gt 200 ]; then ok "matched meson.build runs the real suite and it passes (${dt} ms)"; else bad "real suite rc=$rc in ${dt} ms: $(head -5 "$ERR")"; fi

# --- 6. a failing suite exits 2 with guard names on stderr --------------------------------
BAD="$TMP/bad"
mkdir -p "$BAD/misrc_tools/test"
printf 'import sys\nprint("PASS: something fine")\nprint("FAIL: fake guard contract")\nsys.exit(1)\n' > "$BAD/misrc_tools/test/ci_guard_tests.py"
set +e; run "$BAD/misrc_tools/meson.build" "$BAD"; rc=$?; set -e
if [ "$rc" -eq 2 ] && grep -q 'FAIL: fake guard contract' "$ERR" && grep -q 'reproduce:' "$ERR"; then
  ok "failing suite -> exit 2, guard name and reproduce line on stderr"
else bad "failing suite rc=$rc: $(cat "$ERR")"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
