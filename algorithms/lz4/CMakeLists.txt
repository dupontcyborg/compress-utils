## CMake for 'lz4' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for LZ4
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(LZ4_BUILD_DIR "${ALGORITHMS_DIR}/lz4/build")
set(LZ4_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(LZ4_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/lz4")

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
# Note: LZ4's CMake only uses "lz4_static" name when both shared and static libs are built
# Since we build with BUILD_SHARED_LIBS=OFF, the library is just named "lz4.lib"
if (WIN32)
    set(LZ4_LIB_DIR_GENEX "$<CONFIG>/")  # Generator expression for INSTALL_COMMAND
    set(LZ4_LIB "lz4.lib")
else()
    set(LZ4_LIB_DIR_GENEX "")
    set(LZ4_LIB "liblz4.a")
endif()

# External project for LZ4
include(ExternalProject)
ExternalProject_Add(
    lz4_external
    PREFIX ${LZ4_BUILD_DIR}
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DLZ4_BUILD_LEGACY_LZ4C=OFF
        -DLZ4_BUILD_CLI=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_STATIC_LIBS=ON
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS
        ${LZ4_INSTALL_DIR_LIB}/${LZ4_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${LZ4_INSTALL_DIR_LIB} ${LZ4_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${LZ4_BUILD_DIR}/src/lz4_external-build/${LZ4_LIB_DIR_GENEX}${LZ4_LIB} ${LZ4_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${LZ4_BUILD_DIR}/src/lz4_external/lib/lz4.h ${LZ4_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${LZ4_BUILD_DIR}/src/lz4_external/lib/lz4hc.h ${LZ4_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${LZ4_BUILD_DIR}/src/lz4_external/lib/lz4frame.h ${LZ4_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########


# Create an imported library target for LZ4
add_library(lz4_library STATIC IMPORTED GLOBAL)
set_target_properties(lz4_library PROPERTIES
    IMPORTED_LOCATION ${LZ4_INSTALL_DIR_LIB}/${LZ4_LIB}
)

# Ensure the target is available globally
add_dependencies(lz4_library lz4_external)
