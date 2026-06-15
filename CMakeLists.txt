cmake_minimum_required(VERSION 3.13)

project(inkshelf C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# PocketBook apps are plain ELF binaries renamed to <name>.app and dropped on
# the SD card under system/bin or applications/. We build the binary as
# "inkshelf.app" directly so the artifact is install-ready.
add_executable(inkshelf.app
    src/main.c
    src/app.c
    src/ui.c
    src/screens.c
    src/xml.c
    src/opds.c
    src/opds_ui.c
    src/http.c
    src/library.c
    src/download.c
)

target_include_directories(inkshelf.app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# libcurl provides HTTP for OPDS fetches and book downloads. Shipped in the
# PocketBook SDK toolchain; find it via the sysroot, fall back to -lcurl.
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(inkshelf.app PRIVATE CURL::libcurl)
else()
    find_library(CURL_LIB NAMES curl HINTS ${PB_SDK_ROOT}/lib ${PB_SYSROOT}/usr/lib)
    if(CURL_LIB)
        target_link_libraries(inkshelf.app PRIVATE ${CURL_LIB})
    else()
        message(WARNING "libcurl not found; linking with -lcurl")
        target_link_libraries(inkshelf.app PRIVATE curl)
    endif()
endif()

# InkView is the PocketBook UI/runtime library. When building inside the SDK
# image it is found via the sysroot; allow an explicit hint otherwise.
find_library(INKVIEW_LIB
    NAMES inkview
    HINTS ${PB_SDK_ROOT}/lib ${PB_SYSROOT}/usr/lib
)

if(INKVIEW_LIB)
    target_link_libraries(inkshelf.app PRIVATE ${INKVIEW_LIB})
else()
    # Fall back to a bare -linkview so configuration still succeeds on a host
    # without the SDK present (e.g. for editing / static analysis).
    message(WARNING "libinkview not found via find_library; linking with -linkview")
    target_link_libraries(inkshelf.app PRIVATE inkview)
endif()

# No "lib" prefix, keep the .app suffix exactly.
set_target_properties(inkshelf.app PROPERTIES
    PREFIX ""
    SUFFIX ""
    OUTPUT_NAME "inkshelf.app"
)
