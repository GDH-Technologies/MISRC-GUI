#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

# Single source of truth for the MISRC version string. Resolution order:
#   1. MISRC_TOOLS_VERSION_OVERRIDE / MISRC_TOOLS_VERSION env (CI release runs
#      set this to the tag).
#   2. An exact tag at HEAD (a checked-out release tag, e.g. v1.1.6).
#   3. A date-stamped dev version: dev-YYYY-MM-DD-<sha> (UTC date + short commit
#      SHA), with -dirty appended if the working tree has tracked changes.
#
# Dev builds MUST NOT use a hardcoded "vX.Y.Z-dev" literal. Such a string goes
# stale the moment a release advances past it (the repo previously carried a
# hardcoded vX.Y.Z-dev literal in this script and the VERSION file while the
# current release had advanced past it, so dev builds reported a version
# behind the last release). The date-stamped scheme is always current and the
# SHA pins the exact commit. Tagged releases keep the bare tag name.
V="${MISRC_TOOLS_VERSION_OVERRIDE:-${MISRC_TOOLS_VERSION:-}}"

if [ -z "$V" ]; then
	V=$(git describe --tags --exact-match --match 'v*' --match 'misrc_tools-*' 2>/dev/null || true)
fi

SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")

if [ -z "$V" ]; then
	DATE=$(date -u +%Y-%m-%d)
	V="dev-${DATE}-${SHA}"
fi

# Append -dirty for a modified tree. Tagged releases stay as the bare tag;
# dev builds already include the SHA so only -dirty is appended when needed.
# --ignore-cr-at-eol: on Windows, actions/checkout (System git, autocrlf=true)
# checks out CRLF while MSYS2 git (autocrlf=false) compares raw bytes, so a
# fresh checkout would otherwise phantom-dirty as CRLF-vs-LF. Ignoring CR at
# EOL makes the dirty check see real content changes only; it is a no-op on
# Linux/macOS (LF endings, no CR to ignore).
case "$V" in
	dev-*)
		if git diff --quiet --ignore-submodules --ignore-cr-at-eol -- 2>/dev/null; then
			:
		else
			V="${V}-dirty"
		fi
		;;
	v*|misrc_tools-*)
		: # exact tag — keep as-is
		;;
	*)
		# Any other override string: append SHA + dirty so it stays traceable.
		DIRTY=""
		if git diff --quiet --ignore-submodules --ignore-cr-at-eol -- 2>/dev/null; then :; else DIRTY="-dirty"; fi
		V="${V}-${SHA}${DIRTY}"
		;;
esac

case "$V" in
	misrc_tools-*)
		V="${V#misrc_tools-}"
		;;
esac

printf '%s\n' "$V"
