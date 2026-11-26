#!/usr/bin/env bash
# Mint the fork's next release tag, ${base}-gdh.N, on HEAD and push it (fork-only).
#
# Usage: scripts/gdh-ci/bump-gdh-tag.sh [--dry-run]
#
#   --dry-run   compute and print the tag a merge of HEAD would get, check the checkout's
#               tags against origin's, and stop: no tag, no push. Every same-repo PR runs
#               this on wm (selfhosted-deploy.yml, job tag-preview).
#
# Env (for local testing): REMOTE (default origin), UPSTREAM_URL (default harrypm's repo).
# Run by hand, only --dry-run is useful: a real run pushes with whatever credentials the
# repository's own config carries, and this script reads no global config (below).
#
# Called by selfhosted-deploy.yml's bump-tag job on a push to main, before any build leg
# checks out, so every leg resolves the same version (docs/gdh-selfhosted-ci.md, "The tag
# bump is its own job"). misrc_tools/git-version.sh resolves a version only from an EXACT
# tag at HEAD; without this, main builds report dev-<date>-<sha>.
#
# Rules this script keeps, each learned the hard way:
#
# - It reads no global or system git config. wm's runners run as the desktop user, and
#   actions/checkout swaps HOME only for its own step, so every later step reads that
#   user's ~/.gitconfig. On 2026-11-26 that file gained fetch.prune = true; the old inline
#   step's `git fetch <harrypm> 'refs/tags/v*:refs/tags/v*'` then pruned every local -gdh
#   tag (git prunes the destination of an explicit refspec), counted N from zero, minted
#   v1.2.7-gdh.1 on top of gdh.1-15 and died on the rejected push (the #117 merge). The
#   same file's tag.gpgSign signed the bot's tag with a personal SSH key. The checkout's
#   push credentials live in the repository's local config, which is still read.
#
# - harrypm's release tags never land in refs/tags. The fork's remote deliberately carries
#   none of his plain vX.Y.Z tags (pushing one would fire the workflow's tags: ['v*'] trigger
#   and install upstream's tree), so they are fetched by URL into the private
#   refs/gdh-bump/upstream-tags/ namespace, as upstream-drift.yml does with
#   refs/upstream-drift/. No prune setting can then touch the fork's tags.
#
# - The base is the HIGHEST upstream release tag HEAD contains, never the nearest: a sync
#   merge sits closer to the previous -gdh tag than to the new upstream tag, and
#   `git describe` kept the old base (the v1.1.9 merge, #56, was minted v1.1.8-gdh.22).
#
# - N is one past the highest ${base}-gdh.N on ORIGIN, not in the local checkout. The
#   checkout must still hold every one of origin's tags on that base; if it does not,
#   something rewrote the checkout's tags, and the script fails rather than tagging.
#
# - Every grep below ends a pipeline that may legitimately match nothing; under pipefail
#   that would abort silently (it did, after #57, before any v1.1.9-gdh.* existed), so
#   each one is guarded and the empty case is handled explicitly.
set -euo pipefail

export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1

dry_run=false
case "${1:-}" in
  "") ;;
  --dry-run) dry_run=true ;;
  *) echo "usage: $0 [--dry-run]" >&2; exit 2 ;;
esac

REMOTE="${REMOTE:-origin}"
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/harrypm/MISRC-GUI.git}"
NS="refs/gdh-bump/upstream-tags"
BOT_NAME="github-actions[bot]"
BOT_EMAIL="41898282+github-actions[bot]@users.noreply.github.com"

err() { echo "::error::$*" >&2; exit 1; }

summary() {
  echo "$1"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "$1" >> "$GITHUB_STEP_SUMMARY"
  fi
}

head_sha="$(git rev-parse HEAD)"

# Already tagged (a re-run of the same commit, or a merge a human tagged first): leave it
# alone rather than minting a second tag for one commit.
existing="$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null || true)"
if [ -n "$existing" ]; then
  summary "HEAD ($head_sha) is already $existing; not bumping"
  exit 0
fi

# Upstream release tags, fresh: clear the namespace (a checkout reused across runs would
# otherwise keep a tag upstream deleted), then fetch into it.
git for-each-ref --format='delete %(refname)' "$NS/" | git update-ref --stdin
if ! git fetch --quiet --no-tags --no-prune "$UPSTREAM_URL" "+refs/tags/v*:$NS/v*"; then
  err "could not fetch release tags from $UPSTREAM_URL to base the GDH tag on"
fi

base="$(git for-each-ref --merged HEAD --format='%(refname:lstrip=3)' "$NS/" \
        | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -n 1 || true)"
[ -n "$base" ] || err "no upstream vX.Y.Z tag reachable from HEAD to base a GDH release tag on"

base_re="${base//./\\.}"

# Read origin's ${base}-gdh.N tags into remote_tags (one name per line, empty when none
# exist yet) and set next to one past the highest. Called at top level, never inside $( ),
# so a failed ls-remote stops the script instead of reading as "no tags".
refresh_remote() {
  local listing n
  listing="$(git ls-remote --tags --refs "$REMOTE" "refs/tags/${base}-gdh.*")" \
    || err "could not list tags on $REMOTE"
  remote_tags="$(printf '%s\n' "$listing" | sed -n 's#^[0-9a-f]*[[:space:]]*refs/tags/##p' \
                 | grep -E "^${base_re}-gdh\.[0-9]+$" || true)"
  n="$(printf '%s\n' "$remote_tags" | sed -n "s/^${base_re}-gdh\.//p" | sort -n | tail -n 1)"
  next="${base}-gdh.$(( ${n:-0} + 1 ))"
}

refresh_remote

# The checkout must hold every tag origin has on this base. A missing one means something
# removed tags from the checkout (a prune, a stale workspace), and counting or describing
# from it would be wrong.
missing=""
while IFS= read -r t; do
  [ -n "$t" ] || continue
  git rev-parse -q --verify "refs/tags/$t" >/dev/null || missing="$missing $t"
done <<< "$remote_tags"
if [ -n "$missing" ]; then
  err "the checkout is missing origin's tags:$missing (something removed them; refusing to tag)"
fi

if [ "$dry_run" = true ]; then
  summary "would tag $next at $head_sha (base $base, from origin's tags)"
  exit 0
fi

# Peeled commit of origin's tag $1 into peeled; empty if origin has no such tag.
remote_peeled() {
  local listing
  listing="$(git ls-remote --tags "$REMOTE" "refs/tags/$1")" || err "could not list tags on $REMOTE"
  peeled="$(printf '%s\n' "$listing" \
    | awk -v t="refs/tags/$1" '$2 == t"^{}" { p = $1 } $2 == t { l = $1 } END { print (p != "" ? p : l) }')"
}

for attempt in 1 2 3; do
  if git rev-parse -q --verify "refs/tags/$next" >/dev/null; then
    err "a local tag $next exists but origin has none; delete it or re-run in a fresh checkout"
  fi
  git -c "user.name=$BOT_NAME" -c "user.email=$BOT_EMAIL" \
    tag -a --no-sign "$next" -m "auto: $head_sha"
  if git push --quiet "$REMOTE" "refs/tags/$next"; then
    summary "tagged $next at $head_sha"
    exit 0
  fi
  git tag -d "$next" >/dev/null
  remote_peeled "$next"
  if [ "$peeled" = "$head_sha" ]; then
    summary "origin already has $next at $head_sha; nothing to do"
    exit 0
  fi
  echo "::warning::push of $next failed (attempt $attempt; origin has it at ${peeled:-nothing}); recounting" >&2
  refresh_remote
done
err "could not push a GDH release tag after 3 attempts"
