#!/usr/bin/env bash
#
# inkshelf-build.sh — pull latest source, cross-compile for PocketBook (ARM),
# and copy the binary to a connected device.
#
# Usage:
#   ./inkshelf-build.sh           # build and copy to device
#   ./inkshelf-build.sh --pull    # git pull, then build and copy
#   ./inkshelf-build.sh --no-copy # build only, skip copying to device
#
set -euo pipefail

# ---- paths (edit if your layout differs) ------------------------------------
# Default the project to this script's own directory (the repo root) so it works
# from any clone; override with INKSHELF_DIR. The SDK defaults to the usual
# location; override with PB_SDK_ROOT (same variable build.sh uses).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="${INKSHELF_DIR:-$SCRIPT_DIR}"
SDK="${PB_SDK_ROOT:-$HOME/pocketbook-sdk/SDK-B288}"
SYSROOT="$SDK/usr/arm-obreey-linux-gnueabi/sysroot"
CC="$SDK/usr/bin/arm-obreey-linux-gnueabi-gcc"
CXX="$SDK/usr/bin/arm-obreey-linux-gnueabi-g++"
CACERT_URL="https://curl.se/ca/cacert.pem"
CACERT_LOCAL="$PROJECT/assets/cacert.pem"

DO_PULL=0
DO_COPY=1
for arg in "$@"; do
  case "$arg" in
    --pull)    DO_PULL=1 ;;
    --no-copy) DO_COPY=0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

export PATH="$SDK/usr/bin:$PATH"
cd "$PROJECT"

# ---- 1. (optional) update source --------------------------------------------
if [ "$DO_PULL" = 1 ]; then
  echo ">> git pull"
  git pull
  rm -rf build
fi

# ---- 2. CA bundle (download if missing or older than 30 days) ---------------
mkdir -p "$PROJECT/assets"
if [ ! -f "$CACERT_LOCAL" ] || find "$CACERT_LOCAL" -mtime +30 | grep -q .; then
  echo ">> refreshing CA bundle"
  curl -fsSL "$CACERT_URL" -o "$CACERT_LOCAL"
fi

# ---- 3. cmake configure (only if build/ does not exist yet) -----------------
if [ ! -d build ]; then
  echo ">> cmake configure"
  cmake -B build \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_FIND_ROOT_PATH="$SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
fi

# ---- 4. build ---------------------------------------------------------------
echo ">> build"
cmake --build build

APP="$PROJECT/build/inkshelf.app"

# ---- 5. verify ARM binary ---------------------------------------------------
echo ">> binary check:"
file "$APP"
file "$APP" | grep -q "ELF 32-bit.*ARM" || { echo "!! not an ARM binary"; exit 1; }

# ---- 6. (optional) copy to device -------------------------------------------
if [ "$DO_COPY" = 1 ]; then
  MOUNT=""
  for d in /media/"$USER"/*/; do
    [ -d "${d}applications" ] && MOUNT="$d" && break
  done

  if [ -n "$MOUNT" ]; then
    cp "$APP" "${MOUNT}applications/"
    mkdir -p "${MOUNT}system/config"
    cp "$CACERT_LOCAL" "${MOUNT}system/config/cacert.pem"
    sync
    echo ">> deployed: ${MOUNT}applications/inkshelf.app"
    echo ">> CA bundle: ${MOUNT}system/config/cacert.pem"
    echo ">> safely eject the device before unplugging."
  else
    echo "!! device not mounted under /media/$USER"
    echo "   Connect via USB, allow storage access on the device, then re-run."
  fi
fi

echo ">> done."
