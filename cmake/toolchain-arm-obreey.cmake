# CMake toolchain for PocketBook (Obreey) cross-compilation.
#
# Targets the arm-obreey-linux-gnueabi toolchain shipped with PocketBook
# SDK_6.3.0 (https://github.com/pocketbook/SDK_6.3.0). NOTE: the SDK lives on
# the repo's `6.5` branch (the default `master` branch is only a README); after
# cloning, `git checkout 6.5` to get `SDK-B288/`. The toolchain binaries are
# Linux x86_64 ELF — they only run on a Linux x86_64 host (or emulation), not
# natively on macOS.
#
# PB_SDK_ROOT should point at the SDK-B288 directory (the one containing usr/).
# The real layout is:
#   $PB_SDK_ROOT/usr/bin/arm-obreey-linux-gnueabi-gcc          (compiler)
#   $PB_SDK_ROOT/usr/arm-obreey-linux-gnueabi/sysroot          (inkview.h, libs)
# If the compiler is already on PATH, PB_SDK_ROOT may be left unset.
#
# Override with -DPB_SDK_ROOT=... or the PB_SDK_ROOT env var.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(PB_TARGET "arm-obreey-linux-gnueabi")

# SDK root: env var wins, then cache var.
if(DEFINED ENV{PB_SDK_ROOT})
    set(PB_SDK_ROOT "$ENV{PB_SDK_ROOT}" CACHE PATH "PocketBook SDK-B288 root")
endif()

# Cross compilers. Honour a PB_TOOLCHAIN_PREFIX override for unusual layouts.
if(NOT PB_TOOLCHAIN_PREFIX)
    set(PB_TOOLCHAIN_PREFIX "${PB_TARGET}-")
endif()

# If we know the SDK root, use absolute paths into usr/bin; otherwise fall back
# to bare names resolved via PATH (e.g. inside a container with usr/bin on PATH).
if(PB_SDK_ROOT AND EXISTS "${PB_SDK_ROOT}/usr/bin/${PB_TOOLCHAIN_PREFIX}gcc")
    set(PB_BINDIR "${PB_SDK_ROOT}/usr/bin/")
else()
    set(PB_BINDIR "")
endif()

set(CMAKE_C_COMPILER   "${PB_BINDIR}${PB_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${PB_BINDIR}${PB_TOOLCHAIN_PREFIX}g++")

# Sysroot holding inkview.h and libinkview.so (real SDK layout).
if(PB_SDK_ROOT)
    set(PB_SYSROOT "${PB_SDK_ROOT}/usr/${PB_TARGET}/sysroot" CACHE PATH "PocketBook sysroot")
    if(EXISTS "${PB_SYSROOT}")
        set(CMAKE_SYSROOT "${PB_SYSROOT}")
    endif()
endif()

set(CMAKE_FIND_ROOT_PATH "${PB_SDK_ROOT}" "${PB_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
