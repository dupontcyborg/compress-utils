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

# External project for Brotli
include(ExternalProject)
ExternalProject_Add(
    brotli_external
    PREFIX ${BROTLI_BUILD_DIR}
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
    BUILD_IN_SOURCE 0
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Copy built libraries and headers to dist directory
ExternalProject_Add_Step(brotli_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BROTLI_INSTALL_DIR_LIB} ${BROTLI_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlidec.a ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlienc.a ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${BROTLI_BUILD_DIR}/src/brotli_external-build/libbrotlicommon.a ${BROTLI_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BROTLI_BUILD_DIR}/src/brotli_external/c/include/brotli ${BROTLI_INSTALL_DIR_INCLUDE}
    DEPENDEES install
)

# Create an imported library target for Brotli
add_library(brotli_library STATIC IMPORTED)
set_target_properties(brotli_library PROPERTIES
    IMPORTED_LOCATION ${BROTLI_INSTALL_DIR_LIB}/libbrotlienc.a
    INTERFACE_INCLUDE_DIRECTORIES ${BROTLI_INSTALL_DIR_INCLUDE}
)

# Ensure compress_utils depends on brotli_external
add_dependencies(compress_utils brotli_external)