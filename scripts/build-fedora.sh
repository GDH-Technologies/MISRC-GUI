#!/usr/bin/env bash
#
# Native Fedora build for MISRC (misrc_gui, misrc_capture, misrc_extract).
#
# Why this exists separately from build-appimage-local.sh: that script targets a
# relocatable AppImage and asserts a glibc 2.35 floor (TARGET_MAX_GLIBC), which a
# current Fedora host can never satisfy -- Fedora 44 is glibc 2.43. Its --native
# mode therefore always fails at the glibc assertion, and its container mode is
# Ubuntu-by-design. This script builds against system libraries instead, which is
# what you want for local development: system FLAC 1.5 (Fedora ships it, so the
# CI's bundled-FLAC machinery is unnecessary), system raylib 5.5, system fftw3f.
#
# The only thing built from source is hsdaoh, because no distro packages it and
# the API used here is the MISRC fork (hsdaoh_raw_callback, hsdaoh_extract_metadata,
# ...), not upstream.
#
# Usage:
#   scripts/build-fedora.sh                 # configure + build + smoke test
#   scripts/build-fedora.sh --clean         # wipe .deps and the build dir first
#   scripts/build-fedora.sh --deps-only     # just build/install vendored hsdaoh
#   BUILD_DIR=out scripts/build-fedora.sh   # override the build directory

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-fedora}"
DEPS_PREFIX="$REPO_ROOT/.deps/install"
DEPS_BUILD="$REPO_ROOT/.deps/hsdaoh-build"
HSDAOH_SRC="$REPO_ROOT/third_party/hsdaoh"

CLEAN=0
DEPS_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --clean)     CLEAN=1 ;;
    --deps-only) DEPS_ONLY=1 ;;
    -h|--help)   sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Dependency check
# ---------------------------------------------------------------------------
# fftw-devel (not fftw3f-devel) is the Fedora package providing fftw3f.pc.
# libX11-devel and mesa-libGL-devel are needed because meson.build appends a
# literal -lX11 -lGL for the GUI, independent of what raylib.pc declares.
# libuvc-devel is only used to build vendored hsdaoh, never by meson.
FEDORA_PACKAGES=(
  gcc meson ninja-build cmake pkgconf-pkg-config git nasm
  flac-devel libusb1-devel raylib-devel fftw-devel soxr-devel alsa-lib-devel
  libX11-devel mesa-libGL-devel libuvc-devel
)

log "Checking build dependencies"
missing_tools=()
for tool in gcc meson ninja cmake pkg-config git nasm; do
  command -v "$tool" >/dev/null 2>&1 || missing_tools+=("$tool")
done

missing_pkgs=()
for pc in flac libusb-1.0 raylib fftw3f soxr alsa x11 gl libuvc; do
  pkg-config --exists "$pc" 2>/dev/null || missing_pkgs+=("$pc")
done

if ((${#missing_tools[@]} || ${#missing_pkgs[@]})); then
  echo
  ((${#missing_tools[@]})) && echo "  missing tools:    ${missing_tools[*]}"
  ((${#missing_pkgs[@]}))  && echo "  missing pkgconfig: ${missing_pkgs[*]}"
  die "Install the build dependencies first:

  sudo dnf install -y ${FEDORA_PACKAGES[*]}"
fi

# meson.build hard-errors below libFLAC 1.5.0 (multithreaded encode). Check it
# here so the failure names the package instead of surfacing mid-configure.
flac_version="$(pkg-config --modversion flac)"
log "System libFLAC: $flac_version"
if ! pkg-config --atleast-version=1.5.0 flac; then
  die "libFLAC $flac_version is too old; MISRC requires >= 1.5.0 for multithreaded FLAC encode."
fi

# ---------------------------------------------------------------------------
# Vendored hsdaoh
# ---------------------------------------------------------------------------
if ((CLEAN)); then
  log "Cleaning $BUILD_DIR and .deps"
  rm -rf "$REPO_ROOT/${BUILD_DIR:?}" "$REPO_ROOT/.deps"
fi

[[ -f "$HSDAOH_SRC/CMakeLists.txt" ]] || die "vendored hsdaoh source missing at $HSDAOH_SRC"

log "Building vendored hsdaoh into .deps/install"
# CMAKE_INSTALL_LIBDIR=lib is deliberate: hsdaoh's CMakeLists hardcodes
# libdir=${exec_prefix}/lib into the generated .pc regardless of the install
# dir, so letting GNUInstallDirs pick lib64 on Fedora would ship a .pc that
# points at a directory that does not exist. meson.build now tolerates either,
# but keeping them consistent avoids the rpath fallback entirely.
cmake -S "$HSDAOH_SRC" -B "$DEPS_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DINSTALL_UDEV_RULES=OFF
cmake --build "$DEPS_BUILD" --parallel "$(nproc)"
cmake --install "$DEPS_BUILD"

# The install produces libhsdaoh.pc; meson asks pkg-config for 'hsdaoh'. The
# meson probe accepts either name now, but provide the alias anyway so an
# ad-hoc `PKG_CONFIG_PATH=... pkg-config --cflags hsdaoh` also works by hand.
for pcdir in "$DEPS_PREFIX/lib/pkgconfig" "$DEPS_PREFIX/lib64/pkgconfig"; do
  if [[ -f "$pcdir/libhsdaoh.pc" && ! -f "$pcdir/hsdaoh.pc" ]]; then
    cp "$pcdir/libhsdaoh.pc" "$pcdir/hsdaoh.pc"
    log "Aliased libhsdaoh.pc -> hsdaoh.pc in $pcdir"
  fi
done

((DEPS_ONLY)) && { log "Dependencies built (--deps-only); stopping here."; exit 0; }

# ---------------------------------------------------------------------------
# Configure + build
# ---------------------------------------------------------------------------
log "Configuring meson ($BUILD_DIR)"
# -Dmisrc_gui=enabled so a missing raylib/fftw3f fails here with a clear message
# rather than silently skipping the GUI target.
if [[ -f "$REPO_ROOT/$BUILD_DIR/meson-private/coredata.dat" ]]; then
  meson setup "$REPO_ROOT/$BUILD_DIR" "$REPO_ROOT/misrc_tools" \
    --buildtype release -Dmisrc_gui=enabled --wipe
else
  meson setup "$REPO_ROOT/$BUILD_DIR" "$REPO_ROOT/misrc_tools" \
    --buildtype release -Dmisrc_gui=enabled
fi

log "Building"
meson compile -C "$REPO_ROOT/$BUILD_DIR" misrc_capture misrc_extract misrc_gui

# ---------------------------------------------------------------------------
# Smoke tests (same three the CI/AppImage script uses)
# ---------------------------------------------------------------------------
log "Smoke tests"

# --smoke-test returns 0 before InitWindow, so this is headless-safe.
"$REPO_ROOT/$BUILD_DIR/misrc_gui" --smoke-test
echo "  misrc_gui --smoke-test: ok"

for tool in misrc_capture misrc_extract; do
  out="$("$REPO_ROOT/$BUILD_DIR/$tool" -h 2>&1 || true)"
  rc=0
  "$REPO_ROOT/$BUILD_DIR/$tool" -h >/dev/null 2>&1 || rc=$?
  [[ $rc -eq 1 ]]           || die "$tool -h exited $rc, expected 1"
  grep -q "Usage" <<<"$out" || die "$tool -h did not print a usage block"
  echo "  $tool -h: ok (exit 1, prints Usage)"
done

version="$("$REPO_ROOT/$BUILD_DIR/misrc_gui" --version 2>&1 || true)"
echo "  misrc_gui --version: $version"
[[ "$version" == "unknown_version" ]] && \
  echo "  NOTE: version is unresolved -- check that misrc_tools/git-version.sh is executable."

log "Done. Binaries in $BUILD_DIR/"
printf '  %s\n' "$REPO_ROOT/$BUILD_DIR/misrc_gui" \
                "$REPO_ROOT/$BUILD_DIR/misrc_capture" \
                "$REPO_ROOT/$BUILD_DIR/misrc_extract"
