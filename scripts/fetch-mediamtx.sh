#!/usr/bin/env bash
#
# Fetch a pinned mediamtx and stage it into an AppDir.
#
#   usage: fetch-mediamtx.sh <x86_64|aarch64> <bin_dir> <doc_dir>
#
# Shared by .github/workflows/build.yml and scripts/build-appimage-local.sh so a
# locally built AppImage contains the same server as a released one. Duplicating
# the fetch in both places is how they end up shipping different versions.
#
# The version and both hashes are pinned HERE, in the repo. The release also
# publishes a checksums.sha256, but verifying a download against a file fetched
# from the same place it came from proves only that the transfer worked --
# whoever could alter one could alter the other. Pinning means an upstream
# change to a published artifact fails the build instead of silently shipping.
#
# gui_mediamtx.c looks beside its own binary before it looks at PATH, so a
# mediamtx staged next to misrc_gui is the one an AppImage will use.
set -euo pipefail

MEDIAMTX_VERSION="v1.19.2"
SHA256_x86_64="f9c601cc303ceca8fad2883917b022882672c5bc56311e92dbceb16e5f20c60c"
SHA256_aarch64="562f419912a8668c18216a9e8c95359ec82fbb754e4a44e2953ef62b98eec688"

usage() {
    echo "usage: $(basename "$0") <x86_64|aarch64> <bin_dir> <doc_dir>" >&2
    exit 2
}

[ "$#" -eq 3 ] || usage
ARCH="$1"
BIN_DIR="$2"
DOC_DIR="$3"

case "$ARCH" in
    x86_64)  MTX_ARCH="linux_amd64"; EXPECTED="$SHA256_x86_64" ;;
    aarch64) MTX_ARCH="linux_arm64"; EXPECTED="$SHA256_aarch64" ;;
    *) echo "unsupported arch: $ARCH" >&2; usage ;;
esac

TARBALL="mediamtx_${MEDIAMTX_VERSION}_${MTX_ARCH}.tar.gz"
URL="https://github.com/bluenviron/mediamtx/releases/download/${MEDIAMTX_VERSION}/${TARBALL}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "fetching $TARBALL"
curl --fail --silent --show-error --location --retry 3 -o "$WORK/$TARBALL" "$URL"

echo "verifying against the pinned sha256"
echo "${EXPECTED}  $WORK/$TARBALL" | sha256sum --check --status || {
    echo "CHECKSUM MISMATCH for $TARBALL" >&2
    echo "  expected: $EXPECTED" >&2
    echo "  got:      $(sha256sum "$WORK/$TARBALL" | cut -d' ' -f1)" >&2
    echo "Refusing to bundle an artifact that is not the one this repo pinned." >&2
    exit 1
}

# mediamtx.yml is deliberately not extracted: gui_mediamtx.c generates its own
# at runtime so the ports in the file are always the ports the app chose, and a
# stray shipped config would be a second source of truth.
tar -xzf "$WORK/$TARBALL" -C "$WORK" mediamtx LICENSE

mkdir -p "$BIN_DIR" "$DOC_DIR"
install -m 755 "$WORK/mediamtx" "$BIN_DIR/mediamtx"
install -m 644 "$WORK/LICENSE" "$DOC_DIR/LICENSE"
printf 'mediamtx %s (%s)\nhttps://github.com/bluenviron/mediamtx\n' \
    "$MEDIAMTX_VERSION" "$MTX_ARCH" > "$DOC_DIR/VERSION"

echo "staged mediamtx $MEDIAMTX_VERSION ($MTX_ARCH) -> $BIN_DIR/mediamtx"
