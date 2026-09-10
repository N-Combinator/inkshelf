# CMake toolchain for hard-float PocketBook devices — the RK3566 platform
# (InkPad One, ...).
#
# These readers run an aarch64 kernel with a 32-bit ARM *hard-float* userland
# and ship only /lib/ld-linux-armhf.so.3. The soft-float SDK-B288 build asks for
# /lib/ld-linux.so.3, which does not exist there, so the launcher cannot even
# load it. No 64-bit port is involved: same 32-bit ARM code, different float ABI.
#
# Built with the distribution's cross compiler (Debian/Ubuntu package
# gcc-arm-linux-gnueabihf), deliberately NOT with the toolchain inside the vendor
# SDK: that SDK is only available as a third-party re-upload, so nothing from it
# is allowed to run on the build host or be copied into the binary. The only
# files taken from it are headers and the shared libraries we link against,
# staged by the caller into PB_HF_STAGE:
#
#   $PB_HF_STAGE/include/inkview.h, hwconfig.h, curl/*.h, zlib.h, zconf.h
#   $PB_HF_STAGE/lib/libinkview.so, libcurl.so
#
# The .so files are consulted only for symbol names at link time; on the reader
# the firmware's own libraries are loaded. --allow-shlib-undefined is needed
# because their transitive dependencies (freetype, jpeg, ...) are not staged.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT PB_HF_TRIPLE)
    set(PB_HF_TRIPLE arm-linux-gnueabihf)
endif()
set(CMAKE_C_COMPILER ${PB_HF_TRIPLE}-gcc)

if(DEFINED ENV{PB_HF_STAGE})
    set(PB_HF_STAGE "$ENV{PB_HF_STAGE}" CACHE PATH "Staged vendor headers + link libraries")
endif()
if(NOT PB_HF_STAGE OR NOT EXISTS "${PB_HF_STAGE}/include/inkview.h")
    message(FATAL_ERROR
        "PB_HF_STAGE must point at a staged include/inkview.h + lib/libinkview.so "
        "(see BUILDING.md)")
endif()

# RK3566 = Cortex-A55, running 32-bit ARM code with the hard-float ABI.
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a55 -mfpu=neon-fp-armv8 -mfloat-abi=hard -isystem ${PB_HF_STAGE}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")

# Search the stage and the cross compiler's own armhf libc, never the host's
# native (aarch64/x86_64) libraries — including through pkg-config.
set(CMAKE_FIND_ROOT_PATH "${PB_HF_STAGE}" "/usr/${PB_HF_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${PB_HF_STAGE}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
