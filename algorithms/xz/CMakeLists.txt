cmake_minimum_required(VERSION 3.17)

project(xz_project)

include(ExternalProject)

# Set the source and build directories for XZ
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(XZ_BUILD_DIR "${ALGORITHMS_DIR}/xz/build")

# Set the install directories for the library and headers
set(XZ_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(XZ_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/xz")

# Inherit the build type from the parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)  # Default to Release if none is set
endif()

# Propagate the C/C++ flags, linker flags, and build type from the parent
set(PARENT_C_FLAGS "${CMAKE_C_FLAGS}")
set(PARENT_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(PARENT_LD_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(PARENT_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# Add an external project to download, configure, and build XZ
ExternalProject_Add(
    xz_external
    PREFIX ${XZ_BUILD_DIR}
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG v5.6.3
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${PARENT_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${PARENT_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${PARENT_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${PARENT_LD_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF  # Ensure static build
    BUILD_IN_SOURCE 0  # Build out-of-source (in the build directory)
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

# Copy the built XZ library into the dist/lib directory
ExternalProject_Add_Step(xz_external copy_lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XZ_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${XZ_BUILD_DIR}/src/xz_external-build/liblzma.a ${XZ_INSTALL_DIR_LIB}
    DEPENDEES install
)

# Copy the XZ headers into the dist/include directory
ExternalProject_Add_Step(xz_external copy_headers
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XZ_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${XZ_BUILD_DIR}/src/xz_external/src/liblzma/api/lzma.h ${XZ_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${XZ_BUILD_DIR}/src/xz_external/src/liblzma/api/lzma ${XZ_INSTALL_DIR_INCLUDE}/lzma
    DEPENDEES install
)

# Create an imported library target for xz/lzma after it's built
add_library(xz_library STATIC IMPORTED)

# Specify the location of the library and include files after xz is built
set_target_properties(xz_library PROPERTIES
    IMPORTED_LOCATION ${XZ_INSTALL_DIR_LIB}/liblzma.a
    INTERFACE_INCLUDE_DIRECTORIES ${XZ_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on xz_external so that xz is built before it
add_dependencies(compress_utils xz_external)
