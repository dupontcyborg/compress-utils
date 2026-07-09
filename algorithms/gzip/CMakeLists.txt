## CMake for the `gzip` algorithm dependency.
##
## gzip is not a separate upstream — it's the same zlib library as the `zlib`
## codec, just driven with a different windowBits (RFC 1952 wrapper). It
## therefore reuses ZLIB_URL / ZLIB_TAG from codec-versions.json.
##
## This file is only used by the isolated WASM per-algorithm build (which
## configures one codec at a time in its own staging dir, so there's no clash
## with the `zlib` codec's artifacts). The native build does NOT add this
## subdirectory — it links the `zlib` codec's `zlib_library` directly for gzip
## (see the root CMakeLists), so libz is fetched and built only once there.

######### DEPENDENCY SETUP #########

if(NOT DEFINED CU_ALGO_STAGING_DIR)
    set(CU_ALGO_STAGING_DIR "${CMAKE_SOURCE_DIR}/algorithms")
endif()

# Resolve toolchain to abs path so the child configure can find it.
if(CMAKE_TOOLCHAIN_FILE)
    get_filename_component(_CU_TOOLCHAIN_ABS "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE)
else()
    set(_CU_TOOLCHAIN_ABS "")
endif()

set(ALGORITHMS_DIR "${CU_ALGO_STAGING_DIR}")
set(GZIP_BUILD_DIR "${ALGORITHMS_DIR}/gzip/build")
set(GZIP_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
# The header is zlib.h — our code includes it as "zlib/zlib.h", same as the
# zlib codec — so stage it under the same include subdir.
set(GZIP_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zlib")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# Platform-specific library naming (see the zlib codec's CMakeLists for the
# WASI `libzlibstatic.a` rename rationale).
if (WIN32)
    set(GZIP_LIB_DIR_GENEX "$<CONFIG>/")
    set(GZIP_LIB "zlibstatic.lib")
elseif(_CU_TOOLCHAIN_ABS MATCHES "wasm")
    set(GZIP_LIB_DIR_GENEX "")
    set(GZIP_LIB "libzlibstatic.a")
else()
    set(GZIP_LIB_DIR_GENEX "")
    set(GZIP_LIB "libz.a")
endif()

# External project for zlib (gzip's upstream). Reuses ZLIB_URL / ZLIB_TAG.
include(ExternalProject)
ExternalProject_Add(
    gzip_external
    PREFIX ${GZIP_BUILD_DIR}
    GIT_REPOSITORY ${ZLIB_URL}
    GIT_TAG ${ZLIB_TAG}
    GIT_SHALLOW TRUE
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCMAKE_TOOLCHAIN_FILE=${_CU_TOOLCHAIN_ABS}
        -DBUILD_SHARED_LIBS=OFF
        -DZLIB_BUILD_EXAMPLES=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS
        ${GZIP_INSTALL_DIR_LIB}/${GZIP_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${GZIP_INSTALL_DIR_LIB} ${GZIP_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${GZIP_BUILD_DIR}/src/gzip_external-build/${GZIP_LIB_DIR_GENEX}${GZIP_LIB} ${GZIP_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${GZIP_BUILD_DIR}/src/gzip_external/zlib.h ${GZIP_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${GZIP_BUILD_DIR}/src/gzip_external-build/zconf.h ${GZIP_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########

# Imported library target for gzip (the zlib static archive).
add_library(gzip_library STATIC IMPORTED GLOBAL)
set_target_properties(gzip_library PROPERTIES
    IMPORTED_LOCATION ${GZIP_INSTALL_DIR_LIB}/${GZIP_LIB}
)

add_dependencies(gzip_library gzip_external)
