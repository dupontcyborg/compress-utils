## CMake for 'zstd' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for Zstd. The staging root is the project's source tree
# by default (so the fetch caches across rebuilds), but a cross-compile
# build (e.g. WASM) overrides CU_ALGO_STAGING_DIR before add_subdirectory
# to isolate its build artifacts from the native cache.
if(NOT DEFINED CU_ALGO_STAGING_DIR)
    set(CU_ALGO_STAGING_DIR "${CMAKE_SOURCE_DIR}/algorithms")
endif()
set(ALGORITHMS_DIR "${CU_ALGO_STAGING_DIR}")
set(ZSTD_BUILD_DIR "${ALGORITHMS_DIR}/zstd/build")
set(ZSTD_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZSTD_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zstd")

# Resolve toolchain to an absolute path so the ExternalProject's
# configure step (which runs in its own build dir) can find it.
if(CMAKE_TOOLCHAIN_FILE)
    get_filename_component(_CU_TOOLCHAIN_ABS "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE)
else()
    set(_CU_TOOLCHAIN_ABS "")
endif()

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# Handle Windows-specific library naming
# Note: On Windows with multi-config generators (like Visual Studio), CMAKE_BUILD_TYPE
# is not set at configure time. We use $<CONFIG> generator expression for the build
# directory path (evaluated at build time) but keep a fixed path for BUILD_BYPRODUCTS.
if (WIN32)
    set(ZSTD_LIB_DIR_GENEX "$<CONFIG>/")  # Generator expression for INSTALL_COMMAND
    set(ZSTD_LIB "zstd_static.lib")
else()
    set(ZSTD_LIB_DIR_GENEX "")
    set(ZSTD_LIB "libzstd.a")
endif()

# External project for zstd
include(ExternalProject)
ExternalProject_Add(
    zstd_external
    PREFIX ${ZSTD_BUILD_DIR}
    GIT_REPOSITORY ${ZSTD_URL}
    GIT_TAG ${ZSTD_TAG}
    GIT_SHALLOW TRUE
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        # Forward toolchain for cross-compiles (wasm32-wasi, aarch64, etc.).
        # Absolute path so the ExternalProject's configure step (which
        # runs in its own build dir) can find it. Empty for native builds.
        -DCMAKE_TOOLCHAIN_FILE=${_CU_TOOLCHAIN_ABS}
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_BUILD_SHARED=OFF
        -DZSTD_BUILD_PROGRAMS=OFF
        -DZSTD_BUILD_TESTS=OFF
        -DZSTD_BUILD_CONTRIB=OFF
        # Size: drop features our binding never touches. LEGACY_SUPPORT decodes
        # pre-v0.8 frames (we never emit or read them); DICTBUILDER is the ZDICT
        # training API (we expose no dictionary surface — see src/algorithms/
        # zstd/). Both are dead code for our paths, so removing them is
        # perf-neutral. (ZSTD_LIB_MINIFY is intentionally NOT set here — it
        # trades decode speed for size and belongs behind the benchmark gate.)
        -DZSTD_LEGACY_SUPPORT=OFF
        -DZSTD_BUILD_DICTBUILDER=OFF
    SOURCE_SUBDIR build/cmake
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS 
        ${ZSTD_INSTALL_DIR_LIB}/${ZSTD_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${ZSTD_INSTALL_DIR_LIB} ${ZSTD_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external-build/lib/${ZSTD_LIB_DIR_GENEX}${ZSTD_LIB} ${ZSTD_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy
            ${ZSTD_BUILD_DIR}/src/zstd_external/lib/zstd.h
            ${ZSTD_BUILD_DIR}/src/zstd_external/lib/zstd_errors.h
            ${ZSTD_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########


# Create an imported library target for zstd
add_library(zstd_library STATIC IMPORTED GLOBAL)
set_target_properties(zstd_library PROPERTIES
    IMPORTED_LOCATION ${ZSTD_INSTALL_DIR_LIB}/${ZSTD_LIB}
    # INTERFACE_INCLUDE_DIRECTORIES ${ZSTD_INSTALL_DIR_INCLUDE}
)

# Ensure the target is available globally
add_dependencies(zstd_library zstd_external)