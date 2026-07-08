## CMake for 'brotli' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for Brotli
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
set(BROTLI_BUILD_DIR "${ALGORITHMS_DIR}/brotli/build")
set(BROTLI_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(BROTLI_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/brotli")

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
    set(BROTLI_LIB_DIR_GENEX "$<CONFIG>/")  # Generator expression for INSTALL_COMMAND
    set(BROTLI_LIB_ENC "brotlienc.lib")
    set(BROTLI_LIB_DEC "brotlidec.lib")
    set(BROTLI_LIB_COMMON "brotlicommon.lib")
else()
    set(BROTLI_LIB_DIR_GENEX "")
    set(BROTLI_LIB_ENC "libbrotlienc.a")
    set(BROTLI_LIB_DEC "libbrotlidec.a")
    set(BROTLI_LIB_COMMON "libbrotlicommon.a")
endif()

# Brotli v1.2.0 embeds a ~192 KB encoder-only static-dictionary lookup table
# (kStaticDictionaryWords/Buckets) as const data by default
# (BROTLI_STATIC_INIT=NONE). For the WASM build — where module size is the
# headline metric — switch to EARLY init: a generated `__attribute__((
# constructor))` (run during the reactor's `_initialize`, which the JS loader
# already calls) computes that LUT into zero-init memory at load instead of
# shipping it. Saves ~246 KB / −34% on brotli.wasm, with no ratio change and no
# measured throughput cost (the cost is a one-time init absorbed at load).
# Native builds keep the embedded LUT — their artifact stays byte-identical and
# size isn't a native concern.
#
# The flag must ride inside CMAKE_C_FLAGS: a bare `-DBROTLI_STATIC_INIT=1`
# CMAKE_ARGS element parses as a CMake cache var, not a compiler define.
set(_CU_BROTLI_C_FLAGS "${CMAKE_C_FLAGS}")
if(CU_WASM_TOOLCHAIN)
    set(_CU_BROTLI_C_FLAGS "${_CU_BROTLI_C_FLAGS} -DBROTLI_STATIC_INIT=1")
endif()

# External project for Brotli
include(ExternalProject)
ExternalProject_Add(
    brotli_external
    PREFIX ${BROTLI_BUILD_DIR}
    GIT_REPOSITORY ${BROTLI_URL}
    GIT_TAG ${BROTLI_TAG}
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${_CU_BROTLI_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        # Forward toolchain for cross-compiles (wasm32-wasi, aarch64).
        # Empty for native builds; CMake ignores the empty value.
        -DCMAKE_TOOLCHAIN_FILE=${_CU_TOOLCHAIN_ABS}
        -DBUILD_SHARED_LIBS=OFF
        -DBROTLI_DISABLE_TESTS=ON
        -DBROTLI_BUNDLED_MODE=ON
    BUILD_IN_SOURCE 0
    # Skip the `brotli` CLI target — it pulls in chown/setresuid etc that
    # WASI doesn't have. We only need the three libraries.
    BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG>
        --target brotlienc --target brotlidec --target brotlicommon
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS 
        ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_ENC}
        ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_DEC}
        ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_COMMON}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${BROTLI_INSTALL_DIR_LIB} ${BROTLI_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR_GENEX}${BROTLI_LIB_ENC} ${BROTLI_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR_GENEX}${BROTLI_LIB_DEC} ${BROTLI_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR_GENEX}${BROTLI_LIB_COMMON} ${BROTLI_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy_directory ${BROTLI_BUILD_DIR}/src/brotli_external/c/include/brotli ${BROTLI_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########

# Create imported targets for Brotli libraries
add_library(brotlienc STATIC IMPORTED GLOBAL)
set_target_properties(brotlienc PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_ENC}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

add_library(brotlidec STATIC IMPORTED GLOBAL)
set_target_properties(brotlidec PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_DEC}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

add_library(brotlicommon STATIC IMPORTED GLOBAL)
set_target_properties(brotlicommon PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_COMMON}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

# Ensure the targets are available globally
add_dependencies(brotlienc brotli_external)
add_dependencies(brotlidec brotli_external)
add_dependencies(brotlicommon brotli_external)