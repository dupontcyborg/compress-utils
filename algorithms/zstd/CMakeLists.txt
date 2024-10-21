## CMake for 'zstd' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for Zstd
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(ZSTD_BUILD_DIR "${ALGORITHMS_DIR}/zstd/build")
set(ZSTD_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZSTD_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zstd")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# External project for zstd
include(ExternalProject)
ExternalProject_Add(
    zstd_external
    PREFIX ${ZSTD_BUILD_DIR}
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_BUILD_SHARED=OFF
    SOURCE_SUBDIR build/cmake
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Copy built library and headers to dist directory
ExternalProject_Add_Step(zstd_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZSTD_INSTALL_DIR_LIB} ${ZSTD_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external-build/lib/libzstd.a ${ZSTD_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external/lib/zstd.h ${ZSTD_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for zstd
add_library(zstd_library STATIC IMPORTED)
set_target_properties(zstd_library PROPERTIES
    IMPORTED_LOCATION ${ZSTD_INSTALL_DIR_LIB}/libzstd.a
    INTERFACE_INCLUDE_DIRECTORIES ${ZSTD_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on zstd_external so that zstd is built before it
add_dependencies(compress_utils zstd_external)