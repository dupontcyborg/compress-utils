## CMake for 'xz' algorithm dependency

######### DEPENDENCY SETUP #########

# Set directories for XZ
set(ALGORITHMS_DIR "${CMAKE_SOURCE_DIR}/algorithms")
set(XZ_BUILD_DIR "${ALGORITHMS_DIR}/xz/build")
set(XZ_INSTALL_DIR_LIB "${ALGORITHMS_DIR}/dist/lib")
set(XZ_INSTALL_DIR_INCLUDE "${ALGORITHMS_DIR}/dist/include/xz")

# Inherit build type from parent project
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add -fPIC flag for UNIX-like systems (Linux)
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

# External project for XZ
include(ExternalProject)

# Conditional compiler flags for Windows
if (MSVC)
    set(XZ_C_FLAGS "/wd5105")
    set(XZ_CXX_FLAGS "/wd5105")
else()
    set(XZ_C_FLAGS "")
    set(XZ_CXX_FLAGS "")
endif()

ExternalProject_Add(
    xz_external
    PREFIX ${XZ_BUILD_DIR}
    GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG v5.6.3
    GIT_SHALLOW TRUE
    CMAKE_ARGS 
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}${XZ_C_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
        -DLZMA_API_STATIC=ON
    BUILD_IN_SOURCE 0
    BUILD_COMMAND ${CMAKE_COMMAND} --build . --target liblzma --config $<CONFIG>
    UPDATE_DISCONNECTED 1
    INSTALL_COMMAND ""
)

######### INSTALL #########

# Handle Windows-specific library naming
if (WIN32)
    set(XZ_LIB_DIR ${CMAKE_BUILD_TYPE}/)
    set(XZ_LIB "lzma.lib")
else()
    set(XZ_LIB_DIR "")
    set(XZ_LIB "liblzma.a")
endif()

# Copy built library and headers to dist directory
ExternalProject_Add_Step(xz_external copy_artifacts
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XZ_INSTALL_DIR_LIB} ${XZ_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy ${XZ_BUILD_DIR}/src/xz_external-build/${XZ_LIB_DIR}${XZ_LIB} ${XZ_INSTALL_DIR_LIB}
    COMMAND ${CMAKE_COMMAND} -E copy ${XZ_BUILD_DIR}/src/xz_external/src/liblzma/api/lzma.h ${XZ_INSTALL_DIR_INCLUDE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${XZ_BUILD_DIR}/src/xz_external/src/liblzma/api/lzma ${XZ_INSTALL_DIR_INCLUDE}/lzma
    DEPENDEES install
)

# Create an imported library target for XZ
add_library(xz_library STATIC IMPORTED GLOBAL)
set_target_properties(xz_library PROPERTIES
    IMPORTED_LOCATION ${XZ_INSTALL_DIR_LIB}/${XZ_LIB}
    # INTERFACE_INCLUDE_DIRECTORIES ${XZ_INSTALL_DIR_INCLUDE}
)

# Ensure the target is available globally
add_dependencies(xz_library xz_external)