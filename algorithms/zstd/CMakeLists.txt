cmake_minimum_required(VERSION 3.17)

project(zstd_project)

include(ExternalProject)

# Set the source and build directories for Zstd
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(ZSTD_BUILD_DIR "${ALGORITHMS_DIR}/zstd/build")

# Set the install directories for the library and headers
set(ZSTD_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZSTD_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zstd")

# Inherit the build type from the parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)  # Default to Release if none is set
endif()

# Propagate the C/C++ flags, linker flags, and build type from the parent
set(PARENT_C_FLAGS "${CMAKE_C_FLAGS}")
set(PARENT_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(PARENT_LD_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(PARENT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# Add an external project to download, configure, and build Zstd
ExternalProject_Add(
    zstd_external
    PREFIX ${ZSTD_BUILD_DIR}
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${PARENT_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${PARENT_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${PARENT_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${PARENT_LD_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_BUILD_SHARED=OFF
    SOURCE_SUBDIR build/cmake
    BUILD_IN_SOURCE 0  # Build out-of-source (in the build directory)
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

# Copy the built zstd library into the dist/lib directory
ExternalProject_Add_Step(zstd_external copy_lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZSTD_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external-build/lib/libzstd.a ${ZSTD_INSTALL_DIR_LIB}
    DEPENDEES install
)

# Copy the zstd headers into the dist/include directory
ExternalProject_Add_Step(zstd_external copy_headers
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZSTD_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external/lib/zstd.h ${ZSTD_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for zstd after it's built
add_library(zstd_library STATIC IMPORTED)

# Specify the location of the library and include files after zstd is built
set_target_properties(zstd_library PROPERTIES
    IMPORTED_LOCATION ${ZSTD_INSTALL_DIR_LIB}/libzstd.a
    INTERFACE_INCLUDE_DIRECTORIES ${ZSTD_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on zstd_external so that zstd is built before it
add_dependencies(compress_utils zstd_external)
