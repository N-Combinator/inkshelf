# CMake toolchain for PocketBook (Obreey) cross-compilation.
#
# Targets the arm-obreey-linux-gnueabi toolchain shipped with PocketBook
# SDK_6.3.0 (https://github.com/pocketbook/SDK_6.3.0). Intended to be used
# from inside the SDK Docker image, where the toolchain and InkView sysroot
# already live on PATH / in a known sysroot.
#
# Override the SDK location with -DPB_SDK_ROOT=... or the PB_SDK_ROOT env var.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(PB_TARGET "arm-obreey-linux-gnueabi")

# SDK root: env var wins, then cache var, then the conventional image path.
if(DEFINED ENV{PB_SDK_ROOT})
    set(PB_SDK_ROOT "$ENV{PB_SDK_ROOT}" CACHE PATH "PocketBook SDK root")
endif()
if(NOT PB_SDK_ROOT)
    set(PB_SDK_ROOT "/usr/local/${PB_TARGET}" CACHE PATH "PocketBook SDK root")
endif()

# Cross compilers. Honour a PB_TOOLCHAIN_PREFIX override for unusual layouts.
if(NOT PB_TOOLCHAIN_PREFIX)
    set(PB_TOOLCHAIN_PREFIX "${PB_TARGET}-")
endif()

set(CMAKE_C_COMPILER   "${PB_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${PB_TOOLCHAIN_PREFIX}g++")

# Sysroot holding inkview.h and libinkview.so.
set(PB_SYSROOT "${PB_SDK_ROOT}/sysroot" CACHE PATH "PocketBook sysroot")
if(EXISTS "${PB_SYSROOT}")
    set(CMAKE_SYSROOT "${PB_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH "${PB_SDK_ROOT}" "${PB_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
