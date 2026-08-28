#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

# Shared release/version resolver for CI build jobs. Single source of truth
# for whether a run is a release context and what version string to use.
# Replaces the per-job inline `if [[ "${GITHUB_REF}" == refs/tags/* ]]` logic
# that was duplicated across linux/windows/macos and absent from android.
#
# Inputs (env, set by the workflow step that calls this):
#   GITHUB_REF          - ${{ github.ref }} (refs/tags/<tag> on a tag push,
#                         refs/heads/<branch> on a push, refs/pull/<n>/merge
#                         on a PR).
#   CI_EVENT_NAME       - ${{ github.event_name }} (workflow_dispatch, push,
#                         pull_request, ...).
#   CI_CREATE_RELEASE   - ${{ github.event.inputs.create_release }} ("true"
#                         only for a workflow_dispatch release run; empty
#                         otherwise).
#   CI_RELEASE_TAG      - ${{ github.event.inputs.release_tag }} (the tag to
#                         release, e.g. v1.1.7; only meaningful for a
#                         workflow_dispatch release run).
#
# Resolution:
#   1. Tag push (GITHUB_REF=refs/tags/*): VERSION = the tag (minus the
#      refs/tags/ prefix).
#   2. workflow_dispatch with CI_CREATE_RELEASE=true: VERSION = CI_RELEASE_TAG.
#      CI_RELEASE_TAG MUST be non-empty, else hard-fail (re-dispatch with
#      release_tag=vX.Y.Z).
#   3. Non-release (PR / dev dispatch / non-release push): VERSION =
#      misrc_tools/git-version.sh (date-stamped dev-YYYY-MM-DD-<sha>), with a
#      dev-<date>-nogit fallback if git-version.sh yields nothing.
#
# Hard guard (last line of defense before the per-job build): a release-context
# run must NEVER resolve a dev-* version. If it does, exit 1 so the build job
# fails instead of shipping a dev-named artifact under the release tag.
# Regression this prevents: v1.1.7 shipped linux_MISRC_dev-..._arm64.zip and
# android_MISRC_dev-..._arm64.apk under tag v1.1.7 because the Linux job
# received an empty release_tag input and silently fell through to
# git-version.sh on a --no-tags --depth=1 shallow clone.

GITHUB_REF="${GITHUB_REF:-}"
CI_EVENT_NAME="${CI_EVENT_NAME:-}"
CI_CREATE_RELEASE="${CI_CREATE_RELEASE:-}"
CI_RELEASE_TAG="${CI_RELEASE_TAG:-}"

RELEASE_CONTEXT=0
VERSION=""

# (1) Tag push.
if [ -n "$GITHUB_REF" ]; then
	case "$GITHUB_REF" in
		refs/tags/*)
			RELEASE_CONTEXT=1
			VERSION="${GITHUB_REF#refs/tags/}"
			;;
	esac
fi

# (2) workflow_dispatch release run.
if [ "$RELEASE_CONTEXT" -eq 0 ] \
	&& [ "$CI_EVENT_NAME" = "workflow_dispatch" ] \
	&& [ "$CI_CREATE_RELEASE" = "true" ]; then
	RELEASE_CONTEXT=1
	if [ -z "$CI_RELEASE_TAG" ]; then
		echo "ERROR: create_release=true but release_tag input is empty; re-dispatch with release_tag=vX.Y.Z" >&2
		exit 1
	fi
	VERSION="$CI_RELEASE_TAG"
fi

# (3) Non-release: date-stamped dev string via the shared git-version.sh.
if [ "$RELEASE_CONTEXT" -eq 0 ]; then
	VERSION="$("$SCRIPT_DIR/git-version.sh" 2>/dev/null || true)"
	if [ -z "$VERSION" ]; then
		DATE=$(date -u +%Y-%m-%d)
		VERSION="dev-${DATE}-nogit"
	fi
fi

# Hard guard: a release run must never resolve a dev version.
if [ "$RELEASE_CONTEXT" -eq 1 ]; then
	case "$VERSION" in
		dev-*)
			echo "ERROR: release run resolved a dev version (${VERSION}); refusing to ship a dev-named artifact under the release tag" >&2
			exit 1
			;;
	esac
fi

printf '%s\n' "$VERSION"
