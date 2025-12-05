## CMake for 'bz2' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for bz2
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(BZ2_BUILD_DIR "${ALGORITHMS_DIR}/bz2/build")
set(BZ2_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(BZ2_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/bz2")

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
if (WIN32)
    set(BZ2_LIB_DIR_GENEX "$<CONFIG>/")  # Generator expression for INSTALL_COMMAND
    set(BZ2_LIB "bz2_static.lib")
else()
    set(BZ2_LIB_DIR_GENEX "")
    set(BZ2_LIB "libbz2_static.a")
endif()

# External project for bz2
include(ExternalProject)
ExternalProject_Add(
    bz2_external
    PREFIX ${BZ2_BUILD_DIR}
    GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DENABLE_STATIC_LIB=ON
        -DENABLE_SHARED_LIB=OFF
        -DENABLE_APP=OFF
        -DENABLE_DOCS=OFF
        -DENABLE_EXAMPLES=OFF
        -DENABLE_TESTS=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    BUILD_BYPRODUCTS
        ${BZ2_INSTALL_DIR_LIB}/${BZ2_LIB}
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${BZ2_INSTALL_DIR_LIB} ${BZ2_INSTALL_DIR_INCLUDE} &&
        ${CMAKE_COMMAND} -E copy ${BZ2_BUILD_DIR}/src/bz2_external-build/${BZ2_LIB_DIR_GENEX}${BZ2_LIB} ${BZ2_INSTALL_DIR_LIB} &&
        ${CMAKE_COMMAND} -E copy ${BZ2_BUILD_DIR}/src/bz2_external/bzlib.h ${BZ2_INSTALL_DIR_INCLUDE}
)

######### INSTALL #########


# Create an imported library target for bz2
add_library(bz2_library STATIC IMPORTED GLOBAL)
set_target_properties(bz2_library PROPERTIES
    IMPORTED_LOCATION ${BZ2_INSTALL_DIR_LIB}/${BZ2_LIB}
)

# Ensure the target is available globally
add_dependencies(bz2_library bz2_external)
