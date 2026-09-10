#!/usr/bin/env bash
#
# stage-rk3566-sdk.sh — pull the handful of files the RK3566 (hard-float) build
# links against out of PocketBook's SDK archive, and nothing else.
#
#   tools/stage-rk3566-sdk.sh /path/to/SDK-RK3566-6.11.7z /path/to/stage
#   PB_PLATFORM=rk3566 PB_HF_STAGE=/path/to/stage ...build...
#
# The RK3566 SDK is only available as a third-party re-upload
# (https://github.com/Sean-on-Git/PocketBook-SDK, release 6.11, "files shared
# from PocketBook's official GitLab"). So the archive AND every file taken from
# it are pinned by SHA-256 below: a changed upload fails here instead of
# silently changing what we compile against. None of the SDK's own tools run —
# the build uses the distribution's cross compiler (cmake/toolchain-armhf.cmake).
#
# Checked before these hashes were pinned (2026-09-10):
#   curl/*.h      byte-identical to the GPG-signed curl 8.16.0 release
#   zlib.h        byte-identical to zlib 1.3.1; zconf.h differs only in the two
#                 HAVE_UNISTD_H / HAVE_STDARG_H lines ./configure rewrites
#   inkview.h,    every InkView call we make keeps its 6.3 signature; no inline
#   hwconfig.h    code, constructors, or process/network calls
#   libinkview.so used only for symbol names at link time — the reader loads its
#   libcurl.so    own firmware copies, nothing from these is copied into the app
#   whole tree    ClamAV + heuristic audit clean, extracted with no network
set -euo pipefail

ARCHIVE_SHA256=a92deed99dc3a09df48970467626b19467661795615ecd9307b3aac1382b4307
SYSROOT=arm-buildroot-linux-gnueabihf_sdk-buildroot/arm-buildroot-linux-gnueabihf/sysroot

# sha256  path inside the SDK sysroot  path in the stage
PINNED='
57ce602b45b49bd57ec48a08bba961160d94ade00a3b426e51d175172162e45d  usr/local/include/inkview.h        include/inkview.h
2a3744adcb7d454036f295393d9da2468766714d316afe8af912be2648c00906  usr/local/include/hwconfig.h       include/hwconfig.h
8a5579af72ea4f427ff00a4150f0ccb3fc5c1e4379f726e101133b1ab9fc600c  usr/include/zlib.h                 include/zlib.h
44e543ff4514c7a9a01d648019c2c9a81d1105cbfa3c77f1faf6cd86eafa8abd  usr/include/zconf.h                include/zconf.h
de758ce9ba928e022b2d9ee81359aeeabeb104ae95dadd9e3fac0b81f3767275  usr/local/lib/libinkview.so        lib/libinkview.so
5f3e09255c5ddeedc6b7eda7f697b6bf5366a3f3c1e54bca2e3744e821070199  usr/lib/libcurl.so.4.8.0           lib/libcurl.so
b9e8eb6651d9b880bc21b5906886f902f8efb186b220994c12c4a84ad69d0852  usr/include/curl/curl.h            include/curl/curl.h
4817550b339341133dcdd03cde0704e678f65a0fcfa7ad56eb4ff97294ea6b52  usr/include/curl/curlver.h         include/curl/curlver.h
f6dac9703e0d4b091e0d2e3cc7d43009174be453ba12607f97ce2a743173d404  usr/include/curl/easy.h            include/curl/easy.h
7dff3bd37afb58c009f0755cc1b37401f7c409e58172eaed492c819181a86e3f  usr/include/curl/header.h          include/curl/header.h
a79657fa15db71e732bcb58bc4645ab65bdb3222767f34be5bab56aa0d4f4cc9  usr/include/curl/mprintf.h         include/curl/mprintf.h
9c46c34a7474cd2f0d321d3126155fd262f6a890188cfbbbe398f5ad6573c833  usr/include/curl/multi.h           include/curl/multi.h
03a9c51b7173c8d9f3aeb1a280eedc43c66a8e4d757a0e950fcdb9ea10577e60  usr/include/curl/options.h         include/curl/options.h
d7588b86814a35ffc3766ff6242e6f6705e04401fc9c208a195caff3503af81c  usr/include/curl/stdcheaders.h     include/curl/stdcheaders.h
643fcc9f35631031d24790494e9740eb8d3408555f2e9d59c30cbfff7f3e31c4  usr/include/curl/system.h          include/curl/system.h
dcee0bf0fbadb902508676dd8fd0a8751ecab41b6631b7133f967f4194d4a29d  usr/include/curl/typecheck-gcc.h   include/curl/typecheck-gcc.h
3323c7546b9a93d4664e977b7973094a93cc31f60a1bf3073b17ae79e7ea2f09  usr/include/curl/urlapi.h          include/curl/urlapi.h
e56d2099d28ecbeb5b8d0978a818e0dd76a252c5e47e614a1a86435424413314  usr/include/curl/websockets.h      include/curl/websockets.h
'

archive=${1:?usage: $0 SDK-RK3566-6.11.7z STAGE_DIR}
stage=${2:?usage: $0 SDK-RK3566-6.11.7z STAGE_DIR}
command -v 7z >/dev/null || { echo "error: 7z not found (apt install 7zip)" >&2; exit 1; }

echo ">> verifying archive checksum"
echo "${ARCHIVE_SHA256}  ${archive}" | sha256sum -c --quiet - \
  || { echo "error: ${archive} is not the audited SDK-RK3566-6.11.7z" >&2; exit 1; }

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
paths=$(echo "$PINNED" | awk 'NF==3{print "'"$SYSROOT"'/"$2}')
# shellcheck disable=SC2086
7z x -bso0 -bsp0 -y -o"$tmp" "$archive" $paths

rm -rf "$stage"; mkdir -p "$stage/include/curl" "$stage/lib"
echo "$PINNED" | while read -r sum src dst; do
  [ -n "$sum" ] || continue
  echo "${sum}  $tmp/$SYSROOT/$src" | sha256sum -c --quiet - \
    || { echo "error: $src does not match its pinned checksum" >&2; exit 1; }
  cp "$tmp/$SYSROOT/$src" "$stage/$dst"
done
echo ">> staged $(find "$stage" -type f | wc -l) verified files into $stage"
