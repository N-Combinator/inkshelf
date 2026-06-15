#!/usr/bin/env bash
#
# Cross-compile inkshelf for PocketBook using the official SDK_6.3.0 toolchain.
#
# The PocketBook SDK ships an arm-obreey-linux-gnueabi toolchain plus the
# InkView sysroot. The easiest reproducible way to get them is the SDK Docker
# image. This script runs CMake + make inside that image and drops
# build/inkshelf.app on the host.
#
# Usage:
#   ./build.sh                # build inside the SDK Docker image (default)
#   PB_SDK_IMAGE=myimg ./build.sh
#   NO_DOCKER=1 ./build.sh    # build directly (toolchain already on PATH)
#
# Override the SDK root for a non-Docker build with PB_SDK_ROOT.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT}/build"

# Default to the community PocketBook SDK image; swap via PB_SDK_IMAGE.
PB_SDK_IMAGE="${PB_SDK_IMAGE:-pocketbook/sdk-6.3.0}"

cmake_build() {
    cmake -S "${ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchain-arm-obreey.cmake" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" --parallel
    echo "==> Built: ${BUILD_DIR}/inkshelf.app"
}

if [[ "${NO_DOCKER:-0}" == "1" ]]; then
    cmake_build
else
    echo "==> Building in Docker image: ${PB_SDK_IMAGE}"
    docker run --rm \
        -v "${ROOT}:${ROOT}" \
        -w "${ROOT}" \
        "${PB_SDK_IMAGE}" \
        bash -c "NO_DOCKER=1 '${ROOT}/build.sh'"
fi
