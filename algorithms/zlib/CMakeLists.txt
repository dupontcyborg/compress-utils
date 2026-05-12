## CMake for `zlib` algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for zlib
# Staging root: defaults to source tree (cached across native builds);
# cross-compile (wasm) overrides CU_ALGO_STAGING_DIR before add_subdirectory.
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
set(ZLIB_BUILD_DIR "${ALGORITHMS_DIR}/zlib/build")
set(ZLIB_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZLIB_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zlib")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# Handle platform-specific library naming.
#
# zlib renames `libzlibstatic.a` → `libz.a` only inside an `if(UNIX)` block
# in its own CMakeLists. CMake doesn't treat WASI as UNIX, so the rename
# is skipped under our wasm cross-compile and the file stays as
# `libzlibstatic.a`. Detect by sniffing the toolchain path — crude but
# avoids leaking wasm-awareness into the algorithm subprojects' contract.
if (WIN32)
    set(ZLIB_LIB_DIR_GENEX "$<CONFIG>/")
    set(ZLIB_LIB "zlibstatic.lib")
elseif(_CU_TOOLCHAIN_ABS MATCHES "wasm")
    set(ZLIB_LIB_DIR_GENEX "")
    set(ZLIB_LIB "libzlibstatic.a")
else()
    set(ZLIB_LIB_DIR_GENEX "")
    set(ZLIB_LIB "libz.a")
endif()

# External project for zlib
include(ExternalProject)
ExternalProject_Add(
    zlib_external
    PREFIX ${ZLIB_BUILD_DIR}
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        # Forward toolchain for cross-compiles (wasm32-wasi, aarch64).
        # Empty for native builds; CMake ignores the empty value.
        -DCMAKE_TOOLCHAIN_FILE=${_CU_TOOLCHAIN_ABS}
        -DBUILD_SHARED_LIBS=OFF
        -DZLIB_BUILD_EXAMPLES=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS 
        ${ZLIB_INSTALL_DIR_LIB}/${ZLIB_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${ZLIB_INSTALL_DIR_LIB} ${ZLIB_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/${ZLIB_LIB_DIR_GENEX}${ZLIB_LIB} ${ZLIB_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external/zlib.h ${ZLIB_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/zconf.h ${ZLIB_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########


# Create an imported library target for zlib
add_library(zlib_library STATIC IMPORTED GLOBAL)
set_target_properties(zlib_library PROPERTIES
    IMPORTED_LOCATION ${ZLIB_INSTALL_DIR_LIB}/${ZLIB_LIB}
    # INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INSTALL_DIR_INCLUDE}
)

# Ensure the target is available globally
add_dependencies(zlib_library zlib_external)