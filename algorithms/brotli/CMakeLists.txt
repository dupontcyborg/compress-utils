## CMake for 'brotli' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for Brotli
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(BROTLI_BUILD_DIR "${ALGORITHMS_DIR}/brotli/build")
set(BROTLI_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(BROTLI_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/brotli")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# External project for Brotli
include(ExternalProject)
ExternalProject_Add(
    brotli_external
    PREFIX ${BROTLI_BUILD_DIR}
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
        -DBROTLI_DISABLE_TESTS=ON
        -DBROTLI_BUNDLED_MODE=ON
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Handle Windows-specific library naming
if (WIN32)
    set(BROTLI_LIB_DIR ${CMAKE_BUILD_TYPE}/)
    set(BROTLI_LIB_ENC "brotlienc.lib")
    set(BROTLI_LIB_DEC "brotlidec.lib")
    set(BROTLI_LIB_COMMON "brotlicommon.lib")
else()
    set(BROTLI_LIB_DIR "")
    set(BROTLI_LIB_ENC "libbrotlienc.a")
    set(BROTLI_LIB_DEC "libbrotlidec.a")
    set(BROTLI_LIB_COMMON "libbrotlicommon.a")
endif()

# Copy built libraries and headers to dist directory
ExternalProject_Add_Step(brotli_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BROTLI_INSTALL_DIR_LIB} ${BROTLI_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR}${BROTLI_LIB_ENC} ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR}${BROTLI_LIB_DEC} ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/${BROTLI_LIB_DIR}${BROTLI_LIB_COMMON} ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BROTLI_BUILD_DIR}/src/brotli_external/c/include/brotli ${BROTLI_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create imported targets for Brotli libraries
add_library(brotlienc STATIC IMPORTED GLOBAL)
set_target_properties(brotlienc PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_ENC}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

add_library(brotlidec STATIC IMPORTED GLOBAL)
set_target_properties(brotlidec PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_DEC}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

add_library(brotlicommon STATIC IMPORTED GLOBAL)
set_target_properties(brotlicommon PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/${BROTLI_LIB_COMMON}
    # INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

# Ensure the targets are available globally
add_dependencies(brotlienc brotli_external)
add_dependencies(brotlidec brotli_external)
add_dependencies(brotlicommon brotli_external)