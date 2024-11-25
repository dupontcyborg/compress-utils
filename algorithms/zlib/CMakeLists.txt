## CMake for `zlib` algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for zlib
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(ZLIB_BUILD_DIR "${ALGORITHMS_DIR}/zlib/build")
set(ZLIB_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(ZLIB_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/zlib")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# External project for zlib
include(ExternalProject)
ExternalProject_Add(
    zlib_external
    PREFIX ${ZLIB_BUILD_DIR}
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
        -DZLIB_BUILD_EXAMPLES=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Handle Windows-specific library naming
if (WIN32)
    set(ZLIB_LIB_DIR ${CMAKE_BUILD_TYPE}/)
    set(ZLIB_LIB "zlibstatic.lib")
else()
    set(ZLIB_LIB_DIR "")
    set(ZLIB_LIB "libz.a")
endif()

# Copy built library and headers to dist directory
ExternalProject_Add_Step(zlib_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZLIB_INSTALL_DIR_LIB} ${ZLIB_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/${ZLIB_LIB_DIR}${ZLIB_LIB} ${ZLIB_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external/zlib.h ${ZLIB_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZLIB_BUILD_DIR}/src/zlib_external-build/zconf.h ${ZLIB_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for zlib
add_library(zlib_library STATIC IMPORTED GLOBAL)
set_target_properties(zlib_library PROPERTIES
    IMPORTED_LOCATION ${ZLIB_INSTALL_DIR_LIB}/${ZLIB_LIB}
    # INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INSTALL_DIR_INCLUDE}
)

# Ensure the target is available globally
add_dependencies(zlib_library zlib_external)