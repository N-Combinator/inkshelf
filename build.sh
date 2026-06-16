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
BUILD_DIR="${ROOT}/build"
PB_TARGET="arm-obreey-linux-gnueabi"

cmake_build() {
    # Fail early with a useful message if the cross toolchain isn't reachable.
    # Real SDK layout: $PB_SDK_ROOT/usr/bin/arm-obreey-linux-gnueabi-gcc.
    if ! command -v "${PB_TARGET}-gcc" >/dev/null 2>&1 \
            && [[ ! -x "${PB_SDK_ROOT:-/nonexistent}/usr/bin/${PB_TARGET}-gcc" ]]; then
        echo "error: cross compiler '${PB_TARGET}-gcc' not found." >&2
        echo "       The SDK lives on the repo's '6.5' branch, not master:" >&2
        echo "         git clone https://github.com/pocketbook/SDK_6.3.0 && cd SDK_6.3.0 && git checkout 6.5" >&2
        echo "       then point at the SDK-B288 dir:  PB_SDK_ROOT=/path/to/SDK-B288 ./build.sh" >&2
        echo "       NOTE: the toolchain is Linux x86_64 — on macOS run this inside a" >&2
        echo "       linux/amd64 container (see README), it will not run natively." >&2
        exit 1
    fi
    cmake -S "${ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-arm-obreey.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --parallel
    echo "==> Built: ${BUILD_DIR}/inkshelf.app"
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
