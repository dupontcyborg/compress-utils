## CMake for `zlib` algorithm

cmake_minimum_required(VERSION 3.17)

project(zlib_project)

include(ExternalProject)

# Set the source and build directories for zlib
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(ZLIB_BUILD_DIR "${ALGORITHMS_DIR}/zlib/build")

# Set the install directories for the library and headers
set(ZLIB_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZLIB_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zlib")

# Inherit the build type from the parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)  # Default to Release if none is set
endif()

# Propagate the C/C++ flags, linker flags, and build type from the parent
set(PARENT_C_FLAGS "${CMAKE_C_FLAGS}")
set(PARENT_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(PARENT_LD_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(PARENT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# External project for zlib
ExternalProject_Add(
    zlib_external
    PREFIX ${ZLIB_BUILD_DIR}
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${PARENT_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${PARENT_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${PARENT_LD_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    BUILD_IN_SOURCE 0  # Build out-of-source (in the build directory)
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

# Copy the built zlib library into the dist/lib directory
ExternalProject_Add_Step(zlib_external copy_lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZLIB_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/libz.a ${ZLIB_INSTALL_DIR_LIB}
    DEPENDEES install
)

# Copy the zlib headers into the dist/include directory
ExternalProject_Add_Step(zlib_external copy_headers
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZLIB_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external/zlib.h ${ZLIB_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/zconf.h ${ZLIB_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for zlib after it's built
add_library(zlib_library STATIC IMPORTED)

# Specify the location of the library and include files after zlib is built
set_target_properties(zlib_library PROPERTIES
    IMPORTED_LOCATION ${ZLIB_INSTALL_DIR_LIB}/libz.a
    INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on zlib_external so that zlib is built before it
add_dependencies(compress_utils zlib_external)
