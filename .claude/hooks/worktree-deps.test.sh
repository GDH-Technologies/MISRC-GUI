#!/usr/bin/env bash
# Regression suite for .claude/hooks/worktree-deps.py (WORKTREE-DEPS-CONTRACT v1).
#
# Drives the hook exactly the way Claude Code does -- a JSON payload on stdin -- using
# throwaway fixtures named hooktest-<pid>* under <main>/.claude/worktrees/, and asserts the
# contract: name validation, branch/directory naming, the base ref (origin/main, or
# upstream/main for upstream/*), .deps linking, the dirty gate, the commits-ahead gate, and
# clean removal that also deletes the branch.
#
# Cleans up after itself. A failed run force-removes only its own hooktest-* fixtures --
# the one place --force is acceptable, because the tree was created seconds ago by this
# script and holds nothing.
#
# Run from any checkout of the repo:   bash .claude/hooks/worktree-deps.test.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
HOOK="$HERE/worktree-deps.py"
MAIN="$(dirname "$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir)")"
ROOT="$MAIN/.claude/worktrees"
TAG="hooktest-$$"
ERR="$(mktemp)"
PASS=0
FAIL=0

ok()  { PASS=$((PASS + 1)); printf 'ok   %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$*"; }
say() { printf '     %s\n' "$*"; }

create() { # name -> stdout: hook stdout; rc preserved
  printf '{"hook_event_name":"WorktreeCreate","name":"%s","cwd":"%s","session_id":"%s"}' \
    "$1" "$MAIN" "$TAG" | python3 "$HOOK" 2>"$ERR"
}
remove() { # path
  printf '{"hook_event_name":"WorktreeRemove","worktree_path":"%s"}' "$1" | python3 "$HOOK" 2>"$ERR"
}
last_line() { printf '%s\n' "$1" | tail -n 1; }
head_of() { git -C "$1" rev-parse HEAD; }
branch_of() { git -C "$1" rev-parse --abbrev-ref HEAD; }
has_branch() { git -C "$MAIN" show-ref --verify --quiet "refs/heads/$1"; }

cleanup() {
  rm -f "$ERR"
  for d in "$ROOT/$TAG" "$ROOT/hooktest+x-$$" "$ROOT/upstream+$TAG"; do
    [ -e "$d" ] || continue
    echo "cleanup: force-removing leftover fixture $d"
    git -C "$MAIN" worktree remove --force "$d" 2>/dev/null || rm -rf "$d"
  done
  git -C "$MAIN" worktree prune 2>/dev/null || true
  for b in "claude/$TAG" "hooktest/x-$$" "upstream/$TAG"; do
    if has_branch "$b"; then git -C "$MAIN" branch -D "$b" >/dev/null 2>&1 || true; fi
  done
}
trap cleanup EXIT

echo "hook: $HOOK"
echo "main: $MAIN"
echo

# --- 1. name validation --------------------------------------------------------------
for n in "" "../x" "a/b/c" ".git" "bad name"; do
  set +e; create "$n" >/dev/null; rc=$?; set -e
  if [ "$rc" -eq 2 ]; then ok "refuses name '$n'"; else bad "name '$n' exited $rc, want 2"; fi
done

# --- 2. bare topic ------------------------------------------------------------------
set +e; out="$(create "$TAG")"; rc=$?; set -e
wt="$(last_line "$out")"
if [ "$rc" -eq 0 ]; then ok "create '$TAG' exits 0"; else bad "create exited $rc: $(cat "$ERR")"; fi
if [ "$wt" = "$ROOT/$TAG" ]; then ok "last stdout line is the absolute path"; else bad "path was '$wt'"; fi
if [ -f "$wt/.git" ]; then ok "worktree registered"; else bad "no .git file in $wt"; fi
if [ "$(branch_of "$wt")" = "claude/$TAG" ]; then ok "branch is claude/$TAG"; else bad "branch is $(branch_of "$wt")"; fi
if [ "$(head_of "$wt")" = "$(git -C "$MAIN" rev-parse origin/main)" ]; then ok "HEAD is origin/main"; else bad "HEAD is not origin/main"; fi
if [ -d "$MAIN/.deps/install" ]; then
  if [ -d "$wt/.deps" ] && [ ! -L "$wt/.deps" ]; then ok ".deps/ is a real directory"; else bad ".deps/ is not a real directory"; fi
  if [ -L "$wt/.deps/install" ] && [ "$(readlink -f "$wt/.deps/install")" = "$(readlink -f "$MAIN/.deps/install")" ]; then
    ok ".deps/install links to the main checkout's"
  else bad ".deps/install is not a link to $MAIN/.deps/install"; fi
  if [ -z "$(git -C "$wt" status --porcelain --untracked-files=all)" ]; then ok ".deps does not dirty the tree"; else bad "tree dirty after linking: $(git -C "$wt" status --porcelain --untracked-files=all | head -3)"; fi
else
  say "skip deps checks: $MAIN/.deps/install absent"
  if grep -q 'no install\* prefix' "$ERR"; then ok "warned about the missing prefix"; else bad "no warning about the missing prefix"; fi
fi

# --- 3. re-entry adopts --------------------------------------------------------------
set +e; out="$(create "$TAG")"; rc=$?; set -e
if [ "$rc" -eq 0 ] && [ "$(last_line "$out")" = "$wt" ]; then ok "re-entry returns the same path"; else bad "re-entry rc=$rc path=$(last_line "$out")"; fi

# --- 4. dirty gate -------------------------------------------------------------------
touch "$wt/DIRTY-$TAG"
set +e; remove "$wt" >/dev/null; rc=$?; set -e
if [ "$rc" -eq 2 ] && [ -f "$wt/.git" ]; then ok "remove refuses a dirty tree"; else bad "dirty remove rc=$rc"; fi
rm -f "$wt/DIRTY-$TAG"

# --- 5. commits-ahead gate ------------------------------------------------------------
git -C "$wt" -c user.name=hooktest -c user.email=hooktest@localhost commit --allow-empty -q -m "hooktest: ahead"
set +e; remove "$wt" >/dev/null; rc=$?; set -e
if [ "$rc" -eq 2 ] && [ -f "$wt/.git" ]; then ok "remove refuses a branch ahead of base"; else bad "ahead remove rc=$rc"; fi
git -C "$wt" reset -q --hard HEAD~1

# --- 6. clean remove ----------------------------------------------------------------
set +e; remove "$wt" >/dev/null; rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "clean remove exits 0"; else bad "clean remove rc=$rc: $(cat "$ERR")"; fi
if [ ! -e "$wt" ]; then ok "worktree directory gone"; else bad "$wt still exists"; fi
if ! has_branch "claude/$TAG"; then ok "branch claude/$TAG deleted"; else bad "branch claude/$TAG still exists"; fi

# --- 7. type/topic naming -------------------------------------------------------------
set +e; out="$(create "hooktest/x-$$")"; rc=$?; set -e
wt2="$(last_line "$out")"
if [ "$rc" -eq 0 ] && [ "$wt2" = "$ROOT/hooktest+x-$$" ]; then ok "type/topic -> directory type+topic"; else bad "type/topic rc=$rc path=$wt2"; fi
if [ "$(branch_of "$wt2")" = "hooktest/x-$$" ]; then ok "type/topic -> branch type/topic"; else bad "branch is $(branch_of "$wt2")"; fi
set +e; remove "$wt2" >/dev/null; rc=$?; set -e
if [ "$rc" -eq 0 ] && [ ! -e "$wt2" ]; then ok "type/topic removes cleanly"; else bad "type/topic remove rc=$rc"; fi

# --- 8. upstream/<topic> bases on upstream/main ----------------------------------------
if git -C "$MAIN" rev-parse --verify --quiet upstream/main >/dev/null; then
  set +e; out="$(create "upstream/$TAG")"; rc=$?; set -e
  wt3="$(last_line "$out")"
  if [ "$rc" -eq 0 ]; then ok "upstream/ topic exits 0"; else bad "upstream/ topic rc=$rc: $(cat "$ERR")"; fi
  if [ "$(head_of "$wt3")" = "$(git -C "$MAIN" rev-parse upstream/main)" ]; then ok "upstream/ topic HEAD is upstream/main"; else bad "upstream/ topic HEAD is not upstream/main"; fi
  set +e; remove "$wt3" >/dev/null; rc=$?; set -e
  if [ "$rc" -eq 0 ] && [ ! -e "$wt3" ]; then ok "upstream/ topic removes cleanly"; else bad "upstream/ remove rc=$rc: $(cat "$ERR")"; fi
else
  say "skip upstream checks: no upstream/main ref"
fi

# --- 9. status ---------------------------------------------------------------------
set +e; (cd "$MAIN" && python3 "$HOOK" status >/dev/null 2>&1); rc=$?; set -e
if [ "$rc" -eq 0 ]; then ok "status exits 0"; else bad "status rc=$rc"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
