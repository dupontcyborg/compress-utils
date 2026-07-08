## CMake for 'snappy' algorithm dependency
##
## Snappy is a C++ codec that also ships a C API (snappy-c.h), which is all
## our vtable (src/algorithms/snappy/snappy.c) needs at compile time. The
## static archive carries C++ symbols, so consumers must link a C++ runtime
## — the root CMakeLists handles that when INCLUDE_SNAPPY is on.

######### DEPENDENCY SETUP #########

# Staging root: defaults to source tree (cached across native builds);
# cross-compile (wasm) overrides CU_ALGO_STAGING_DIR before add_subdirectory.
if(NOT DEFINED CU_ALGO_STAGING_DIR)
    set(CU_ALGO_STAGING_DIR "${CMAKE_SOURCE_DIR}/algorithms")
endif()
set(ALGORITHMS_DIR "${CU_ALGO_STAGING_DIR}")
set(SNAPPY_BUILD_DIR "${ALGORITHMS_DIR}/snappy/build")
set(SNAPPY_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(SNAPPY_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/snappy")

# Resolve toolchain to an absolute path so the child configure can find it.
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

# Windows-specific static library naming.
if (WIN32)
    set(SNAPPY_LIB_DIR_GENEX "$<CONFIG>/")
    set(SNAPPY_LIB "snappy.lib")
else()
    set(SNAPPY_LIB_DIR_GENEX "")
    set(SNAPPY_LIB "libsnappy.a")
endif()

# External project for Snappy
include(ExternalProject)
ExternalProject_Add(
    snappy_external
    PREFIX ${SNAPPY_BUILD_DIR}
    GIT_REPOSITORY ${SNAPPY_URL}
    GIT_TAG ${SNAPPY_TAG}
    GIT_SHALLOW TRUE
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        # Forward toolchain for cross-compiles (wasm32-wasi, aarch64, etc.).
        # Empty for native builds; CMake ignores the empty value.
        -DCMAKE_TOOLCHAIN_FILE=${_CU_TOOLCHAIN_ABS}
        -DBUILD_SHARED_LIBS=OFF
        -DSNAPPY_BUILD_TESTS=OFF
        -DSNAPPY_BUILD_BENCHMARKS=OFF
        -DSNAPPY_INSTALL=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS
        ${SNAPPY_INSTALL_DIR_LIB}/${SNAPPY_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${SNAPPY_INSTALL_DIR_LIB} ${SNAPPY_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${SNAPPY_BUILD_DIR}/src/snappy_external-build/${SNAPPY_LIB_DIR_GENEX}${SNAPPY_LIB} ${SNAPPY_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${SNAPPY_BUILD_DIR}/src/snappy_external/snappy-c.h ${SNAPPY_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########

# Create an imported library target for Snappy
add_library(snappy_library STATIC IMPORTED GLOBAL)
set_target_properties(snappy_library PROPERTIES
    IMPORTED_LOCATION ${SNAPPY_INSTALL_DIR_LIB}/${SNAPPY_LIB}
)

# Ensure the target is available globally
add_dependencies(snappy_library snappy_external)
