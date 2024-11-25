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

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
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
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_BUILD_SHARED=OFF
        -DZSTD_BUILD_PROGRAMS=OFF  
        -DZSTD_BUILD_TESTS=OFF     
        -DZSTD_BUILD_CONTRIB=OFF
    SOURCE_SUBDIR build/cmake
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Handle Windows-specific library naming
if (WIN32)
    set(ZSTD_LIB_DIR ${CMAKE_BUILD_TYPE}/)
    set(ZSTD_LIB "zstd_static.lib")
else()
    set(ZSTD_LIB_DIR "")
    set(ZSTD_LIB "libzstd.a")
endif()

# Copy built library and headers to dist directory
ExternalProject_Add_Step(zstd_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZSTD_INSTALL_DIR_LIB} ${ZSTD_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external-build/lib/${ZSTD_LIB_DIR}${ZSTD_LIB} ${ZSTD_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${ZSTD_BUILD_DIR}/src/zstd_external/lib/zstd.h ${ZSTD_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for zstd
add_library(zstd_library STATIC IMPORTED GLOBAL)
set_target_properties(zstd_library PROPERTIES
    IMPORTED_LOCATION ${ZSTD_INSTALL_DIR_LIB}/${ZSTD_LIB}
    # INTERFACE_INCLUDE_DIRECTORIES ${ZSTD_INSTALL_DIR_INCLUDE}
)

# Ensure the target is available globally
add_dependencies(zstd_library zstd_external)