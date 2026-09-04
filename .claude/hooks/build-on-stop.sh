#!/usr/bin/env bash
# Stop hook: before a turn may end, if any C source under misrc_tools/ differs from
# origin/main (committed on this branch or uncommitted) and this checkout has a configured
# build-local/, rebuild misrc_gui incrementally and run --smoke-test. Exit 2 blocks the stop
# and puts the failure on stderr for the model -- superpowers:verification-before-completion
# as a mechanism rather than a reminder. Exit 0 when nothing changed, when there is no build
# dir (fresh worktree, docs-only work), or when the build and smoke test pass.
#
# Reads the Stop payload on stdin for `cwd`, so a worktree session builds its own tree.
# ninja is a no-op in ~1 s when the tree is already built, so re-running on every stop while
# a C change exists on the branch is cheap. MISRC_BUILD_ON_STOP=0 disables it for a session.
set -uo pipefail

[ "${MISRC_BUILD_ON_STOP:-1}" = "0" ] && exit 0

payload="$(cat 2>/dev/null || true)"
cwd="$(printf '%s' "$payload" | python3 -c 'import json, sys
try:
    print(json.load(sys.stdin).get("cwd") or "")
except Exception:
    print("")' 2>/dev/null || true)"
root="${cwd:-${CLAUDE_PROJECT_DIR:-$PWD}}"
[ -d "$root" ] || exit 0
cd "$root" || exit 0
[ -f build-local/build.ninja ] || exit 0
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0

base="origin/main"
git rev-parse --verify --quiet "$base" >/dev/null 2>&1 || base="HEAD"
# git pathspecs: '*' also matches '/', so these cover every nesting level.
changed="$( { git diff --name-only "$base" -- 'misrc_tools/*.c' 'misrc_tools/*.h'
              git ls-files --others --exclude-standard -- 'misrc_tools/*.c' 'misrc_tools/*.h'; } 2>/dev/null )"
[ -n "$changed" ] || exit 0

log="build-local/.build-on-stop.log"
if ! ninja -C build-local misrc_gui > "$log" 2>&1; then
  echo "[build-on-stop] incremental build FAILED for changed C sources (log: $root/$log)" >&2
  grep -E 'error:|FAILED:|undefined reference|ninja: error' "$log" | head -15 >&2
  exit 2
fi
if ! build-local/misrc_gui --smoke-test >> "$log" 2>&1; then
  echo "[build-on-stop] --smoke-test FAILED after rebuild (log: $root/$log)" >&2
  tail -15 "$log" >&2
  exit 2
fi
exit 0
