#!/usr/bin/env bash
# package.sh PLATFORM BUILD_DIR — zip the built app for one platform.
#
# GitHub rejects release assets whose name ends in .app, hence the zip. The
# checksum file lists the zip (what `sha256sum -c --ignore-missing` checks after
# a download) and, as a comment, the .app inside it — every platform's app has
# the same file name, so it cannot be a checkable entry of the merged file.
set -euo pipefail
platform=$1; build_dir=$2
app=inkshelf.app
ver=$(git describe --tags --always --match 'v*' 2>/dev/null || echo dev)
zip="inkshelf-${ver}-${platform}.zip"
mkdir -p dist
zip -j "dist/$zip" "$build_dir/$app"
(
  cd dist
  sha256sum "$zip"
  echo "# $app inside $zip: $(sha256sum "../$build_dir/$app" | cut -d' ' -f1)"
) > "dist/SHA256SUMS-${platform}.txt"
cat "dist/SHA256SUMS-${platform}.txt"
{
  echo "### $zip"
  echo
  echo '```'
  cat "dist/SHA256SUMS-${platform}.txt"
  echo '```'
  echo
  file -b "$build_dir/$app"
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
