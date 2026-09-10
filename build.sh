#!/usr/bin/env bash
#
# Cross-compile inkshelf for PocketBook using the official SDK_6.3.0 toolchain
# (arm-obreey-linux-gnueabi gcc + InkView sysroot).
#
# There is NO public prebuilt PocketBook SDK Docker image, so this script does
# NOT pull one. You build the .app one of two ways:
#
#   1. Direct build (default) — you have the SDK toolchain installed and either
#      on PATH or under $PB_SDK_ROOT:
#         PB_SDK_ROOT=/path/to/sdk ./build.sh
#      (or just ./build.sh if arm-obreey-linux-gnueabi-gcc is already on PATH)
#
#   2. Docker build — you built your OWN image wrapping the SDK and pass it
#      explicitly:
#         USE_DOCKER=1 PB_SDK_IMAGE=my-pb-sdk:6.3.0 ./build.sh
#
# Get the SDK from https://github.com/pocketbook/SDK_6.3.0 (branch 6.5/master).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PB_TARGET="arm-obreey-linux-gnueabi"

# Which PocketBook platform to build for:
#   b288   (default) soft-float SDK-B288 build — every model except the ones below
#   rk3566 hard-float build for the RK3566 readers (InkPad One, ...), whose
#          32-bit userland only has /lib/ld-linux-armhf.so.3 and so cannot load
#          the soft-float binary at all. See cmake/toolchain-armhf.cmake.
# Each platform gets its own build directory, so switching between them never
# reuses a CMake cache configured for the other toolchain.
PB_PLATFORM="${PB_PLATFORM:-b288}"
case "${PB_PLATFORM}" in
    b288)   BUILD_DIR="${BUILD_DIR:-${ROOT}/build}" ;;
    rk3566) BUILD_DIR="${BUILD_DIR:-${ROOT}/build-rk3566}" ;;
    *)      echo "error: unknown PB_PLATFORM '${PB_PLATFORM}' (expected b288 or rk3566)" >&2; exit 2 ;;
esac

# The SDK's cc1 is a 2017 gcc 6.3 binary that needs libmpfr.so.4 (mpfr 3.x).
# Distros have shipped libmpfr.so.6 for years and package no compatible .so.4,
# so on a current host the compiler dies with "error while loading shared
# libraries: libmpfr.so.4". The SDK bundles the right one under usr/lib, but
# that directory also holds 2017 builds of glib/icu/expat: putting all of it on
# LD_LIBRARY_PATH can break the host's own cmake/make. Link only the
# compiler's own dependencies into a private directory and point at that.
# Kept outside build/ so it cannot interfere with build-directory bookkeeping.
pb_host_libs() {
    [[ -n "${PB_SDK_ROOT:-}" && -d "${PB_SDK_ROOT}/usr/lib" ]] || return 0
    local shim="${ROOT}/.pb-hostlibs" lib
    mkdir -p "${shim}"
    for lib in libmpfr.so.4 libmpc.so.3 libgmp.so.10; do
        if [[ -e "${PB_SDK_ROOT}/usr/lib/${lib}" ]]; then
            ln -sfn "${PB_SDK_ROOT}/usr/lib/${lib}" "${shim}/${lib}"
        fi
    done
    export LD_LIBRARY_PATH="${shim}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}

# RK3566: the distribution's armhf cross compiler plus headers and link
# libraries staged from the vendor SDK. Nothing from that SDK is executed.
cmake_build_rk3566() {
    if ! command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
        echo "error: arm-linux-gnueabihf-gcc not found." >&2
        echo "       Install the distribution cross compiler: apt install gcc-arm-linux-gnueabihf" >&2
        exit 1
    fi
    if [[ ! -f "${PB_HF_STAGE:-/nonexistent}/include/inkview.h" ]]; then
        echo "error: PB_HF_STAGE must point at the staged RK3566 SDK headers + libraries." >&2
        echo "       Create it with:  tools/stage-rk3566-sdk.sh /path/to/SDK-RK3566-6.11.7z <stage-dir>" >&2
        exit 1
    fi
    cmake -S "${ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-armhf.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --parallel
    echo "==> Built: ${BUILD_DIR}/inkshelf.app (RK3566, hard-float)"
    echo "==> Install: copy it to the reader's  applications/  folder."
}

cmake_build() {
    if [[ "${PB_PLATFORM}" == rk3566 ]]; then
        cmake_build_rk3566
        return
    fi
    # Fail early with a useful message if the cross toolchain isn't reachable.
    # Real SDK layout: $PB_SDK_ROOT/usr/bin/arm-obreey-linux-gnueabi-gcc.
    if ! command -v "${PB_TARGET}-gcc" >/dev/null 2>&1 \
            && [[ ! -x "${PB_SDK_ROOT:-/nonexistent}/usr/bin/${PB_TARGET}-gcc" ]]; then
        echo "error: cross compiler '${PB_TARGET}-gcc' not found." >&2
        echo "       The SDK lives on the repo's '6.5' branch, not master:" >&2
        echo "         git clone https://github.com/pocketbook/SDK_6.3.0 && cd SDK_6.3.0 && git checkout 6.5" >&2
        echo "       then point at the SDK-B288 dir:  PB_SDK_ROOT=/path/to/SDK-B288 ./build.sh" >&2
        echo "       NOTE: the toolchain is Linux x86_64 — on macOS run this inside a" >&2
        echo "       linux/amd64 container (see BUILDING.md), it will not run natively." >&2
        exit 1
    fi
    pb_host_libs
    cmake -S "${ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-arm-obreey.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --parallel
    echo "==> Built: ${BUILD_DIR}/inkshelf.app"
    echo "==> Install: copy it to the reader's  applications/  folder."
}

if [[ "${USE_DOCKER:-0}" != "1" ]]; then
    cmake_build
    exit 0
fi

# --- Docker path (opt-in) -------------------------------------------------
if [[ -z "${PB_SDK_IMAGE:-}" ]]; then
    echo "error: USE_DOCKER=1 requires PB_SDK_IMAGE=<your SDK image>." >&2
    echo "       There is no public PocketBook SDK image to pull; build your own" >&2
    echo "       from https://github.com/pocketbook/SDK_6.3.0 and pass it here." >&2
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "error: cannot reach the Docker daemon. Is Docker running?" >&2
    echo "       (macOS: start Docker Desktop, then re-run.)" >&2
    exit 1
fi
if ! docker image inspect "${PB_SDK_IMAGE}" >/dev/null 2>&1; then
    echo "error: Docker image '${PB_SDK_IMAGE}' not found locally and is not" >&2
    echo "       a public image. Build it from the SDK repo first." >&2
    exit 1
fi

echo "==> Building in Docker image: ${PB_SDK_IMAGE}"
docker run --rm \
    -v "${ROOT}:${ROOT}" \
    -w "${ROOT}" \
    "${PB_SDK_IMAGE}" \
    bash -c "'${ROOT}/build.sh'"
