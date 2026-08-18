#!/bin/sh
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
VERSION_FILE="$REPO_ROOT/VERSION"

V="${MISRC_TOOLS_VERSION_OVERRIDE:-${MISRC_TOOLS_VERSION:-}}"

if [ -z "$V" ]; then
	V=$(git describe --tags --exact-match --match 'v*' --match 'misrc_tools-*' 2>/dev/null || true)
fi

if [ -z "$V" ]; then
	if [ -f "$VERSION_FILE" ]; then
		V=$(sed -n '1s/[[:space:]]*$//;1p' "$VERSION_FILE")
	fi
fi

if [ -z "$V" ]; then
	V="v1.1.4-dev"
fi

if [ -n "$V" ]; then
	SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")
	DIRTY=""
	if git diff --quiet --ignore-submodules -- 2>/dev/null; then
		:
	else
		DIRTY="-dirty"
	fi
	case "$V" in
		*v[0-9]*.[0-9]*.[0-9]*-dev*)
			V="${V}-${SHA}${DIRTY}"
			;;
		*)
			# Exact tag builds should stay as tag names.
			case "$V" in
				v*|misrc_tools-*)
					:
					;;
				*)
					V="${V}-${SHA}${DIRTY}"
					;;
			esac
			;;
	esac
fi

case "$V" in
	misrc_tools-*)
		V="${V#misrc_tools-}"
		;;
esac

printf '%s\n' "$V"
