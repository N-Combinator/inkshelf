#!/usr/bin/env bash
#
# Build and run the host-side unit tests for the dependency-free parsing
# code (xml.c + opds.c). These need no InkView and no libcurl, so they run
# on any machine with a C compiler — used as a fast correctness gate before
# cross-compiling for the device.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-cc}"
OUT="$(mktemp -d)"
trap 'rm -rf "${OUT}"' EXIT

"${CC}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"${ROOT}/src" \
    "${ROOT}/src/xml.c" "${ROOT}/src/opds.c" "${ROOT}/tests/test_opds.c" \
    -o "${OUT}/test_opds"

"${OUT}/test_opds"
