cmake_minimum_required(VERSION 3.17)

project(brotli_project)

include(ExternalProject)

# Set the source and build directories for Brotli
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(BROTLI_BUILD_DIR "${ALGORITHMS_DIR}/brotli/build")

# Set the install directories for the library and headers
set(BROTLI_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(BROTLI_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/brotli")

# Inherit the build type from the parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)  # Default to Release if none is set
endif()

# Propagate the C/C++ flags, linker flags, and build type from the parent
set(PARENT_C_FLAGS "${CMAKE_C_FLAGS}")
set(PARENT_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(PARENT_LD_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(PARENT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# Add an external project to download, configure, and build Brotli
ExternalProject_Add(
    brotli_external
    PREFIX ${BROTLI_BUILD_DIR}
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0  # Specify the desired version
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${PARENT_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${PARENT_C_FLAGS}
        # -DCMAKE_CXX_FLAGS=${PARENT_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${PARENT_LD_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
    BUILD_IN_SOURCE 0  # Build out-of-source (in the build directory)
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

# Copy the built brotli library into the dist/lib directory
ExternalProject_Add_Step(brotli_external copy_lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlidec.a ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlienc.a ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlicommon.a ${BROTLI_INSTALL_DIR_LIB}
    DEPENDEES install
)

# Copy the brotli headers into the dist/include directory
ExternalProject_Add_Step(brotli_external copy_headers
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BROTLI_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BROTLI_BUILD_DIR}/src/brotli_external/c/include/brotli ${BROTLI_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for brotli after it's built
add_library(brotli_library STATIC IMPORTED)

# Specify the location of the library and include files after brotli is built
set_target_properties(brotli_library PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/libbrotlienc.a
    INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on brotli_external so that brotli is built before it
add_dependencies(compress_utils brotli_external)
