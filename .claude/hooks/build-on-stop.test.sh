#!/usr/bin/env bash
# Regression suite for build-on-stop.sh.
#
# Drives the hook with a Stop payload on stdin against throwaway git repos and PATH shims for
# ninja / misrc_gui, so every branch is exercised without a real build: disabled by env; no
# build dir; no C changes; failing build -> exit 2; passing build + passing smoke -> exit 0;
# failing smoke -> exit 2. A final case runs against this checkout and expects exit 0 (fast
# when it has no C changes versus origin/main; a real incremental build + smoke when it has).
#
# Run from any checkout:   bash .claude/hooks/build-on-stop.test.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HOOK="$HERE/build-on-stop.sh"
ROOT="$(cd -- "$HERE/../.." && pwd)"
ERR="$(mktemp)"
TMP="$(mktemp -d)"
trap 'rm -rf "$ERR" "$TMP"' EXIT
PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$*"; }
run() { # cwd
  printf '{"hook_event_name":"Stop","cwd":"%s","stop_hook_active":false}' "$1" | "$HOOK" 2>"$ERR"
}
mkrepo() { # dir -> a git repo with one empty commit (the hook needs a HEAD to diff against)
  mkdir -p "$1" && git -C "$1" init -q && git -C "$1" -c user.name=t -c user.email=t@localhost commit -q --allow-empty -m init
}

echo "hook: $HOOK"
echo

# --- 1. disabled by environment ----------------------------------------------------------
set +e; MISRC_BUILD_ON_STOP=0 run "$ROOT"; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "MISRC_BUILD_ON_STOP=0 exits 0"; else bad "disable rc=$rc"; fi

# --- 2. no build dir -> nothing to do ---------------------------------------------------------
mkrepo "$TMP/nobuild"; mkdir -p "$TMP/nobuild/misrc_tools"; echo 'int a;' > "$TMP/nobuild/misrc_tools/a.c"
set +e; run "$TMP/nobuild"; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "no build-local/build.ninja exits 0 even with C changes"; else bad "no-build rc=$rc"; fi

# --- 3. no C changes -> nothing to do -----------------------------------------------------------
mkrepo "$TMP/clean"; mkdir -p "$TMP/clean/build-local"; : > "$TMP/clean/build-local/build.ninja"
set +e; run "$TMP/clean"; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "build dir but no C changes exits 0"; else bad "clean rc=$rc"; fi

# --- shims for the remaining cases -------------------------------------------------------------
R="$TMP/rig"; mkrepo "$R"; mkdir -p "$R/build-local" "$R/misrc_tools" "$TMP/bin"
: > "$R/build-local/build.ninja"; echo 'int x;' > "$R/misrc_tools/x.c"   # an untracked C change

# --- 4. failing build -> exit 2 with the error lines --------------------------------------------
printf '#!/usr/bin/env bash\necho "FAILED: misrc_gui.p/x.c.o"; echo "x.c:1:1: error: fake"; exit 1\n' > "$TMP/bin/ninja"; chmod +x "$TMP/bin/ninja"
set +e; PATH="$TMP/bin:$PATH" run "$R"; rc=$?; set -e
if [ "$rc" -eq 2 ] && grep -q 'incremental build FAILED' "$ERR" && grep -q 'error: fake' "$ERR"; then ok "failing build -> exit 2 with error lines"; else bad "failing build rc=$rc: $(cat "$ERR")"; fi

# --- 5. passing build + passing smoke -> exit 0, silent ------------------------------------------
printf '#!/usr/bin/env bash\nexit 0\n' > "$TMP/bin/ninja"
printf '#!/usr/bin/env bash\n[ "${1:-}" = "--smoke-test" ] && exit 0\nexit 1\n' > "$R/build-local/misrc_gui"; chmod +x "$R/build-local/misrc_gui"
set +e; PATH="$TMP/bin:$PATH" run "$R"; rc=$?; set -e
if [ "$rc" -eq 0 ] && [ ! -s "$ERR" ]; then ok "passing build + smoke -> exit 0, silent"; else bad "pass case rc=$rc: $(cat "$ERR")"; fi

# --- 6. failing smoke test -> exit 2 ------------------------------------------------------------
printf '#!/usr/bin/env bash\necho "[smoke] init failed: fake"; exit 3\n' > "$R/build-local/misrc_gui"
set +e; PATH="$TMP/bin:$PATH" run "$R"; rc=$?; set -e
if [ "$rc" -eq 2 ] && grep -q 'smoke-test FAILED' "$ERR" && grep -q 'init failed: fake' "$ERR"; then ok "failing smoke test -> exit 2 with its output"; else bad "smoke case rc=$rc: $(cat "$ERR")"; fi

# --- 7. this checkout ---------------------------------------------------------------------------
if [ -f "$ROOT/build-local/build.ninja" ]; then
  base=origin/main; git -C "$ROOT" rev-parse --verify --quiet "$base" >/dev/null || base=HEAD
  n=$(git -C "$ROOT" diff --name-only "$base" -- 'misrc_tools/*.c' 'misrc_tools/*.h' | wc -l)
  t0=$(date +%s); set +e; run "$ROOT"; rc=$?; set -e; dt=$(( $(date +%s) - t0 ))
  if [ "$rc" -eq 0 ]; then ok "this checkout ($n changed C file(s) vs $base): exit 0 in ${dt}s"; else bad "this checkout rc=$rc: $(head -5 "$ERR")"; fi
else
  printf '     skip: %s has no build-local/build.ninja\n' "$ROOT"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
