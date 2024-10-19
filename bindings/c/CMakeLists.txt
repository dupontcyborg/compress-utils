## CMake for C language bindings

######### BINDING SETUP #########

# Set C standard
set(CMAKE_C_STANDARD 17)

# Inherit flags from the parent (for LTO, dead code elimination, etc.)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

# Set the C binding directory
set(C_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/c")

# Set the build directory for the C binding
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${C_BINDING_DIR}/build")

# Add the C binding as a shared library
add_library(compress_utils_c SHARED compression_utils_c.cpp)

# Add the C binding as a static library
add_library(compress_utils_c_static STATIC compression_utils_c.cpp)

# Add the include directories for the C binding
target_include_directories(compress_utils_c PRIVATE ${C_BINDING_DIR})
target_include_directories(compress_utils_c_static PRIVATE ${C_BINDING_DIR})

# Link the C++ core library (static) with the C binding shared library
target_link_libraries(compress_utils_c PRIVATE compress_utils_static)

# Link the C++ core library (static) with the C binding static library
target_link_libraries(compress_utils_c_static PRIVATE compress_utils_static)

######### INSTALL #########

# Set the install directories for the C binding
set(C_DIST_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/c/lib")
set(C_DIST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dist/c/include/compression_utils_c")

# Configure the header file
configure_file(
    ${C_BINDING_DIR}/algorithms.h.in
    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h
    @ONLY
)

# Install the C binding shared library
install(TARGETS compress_utils_c
        LIBRARY DESTINATION ${C_DIST_LIB_DIR}
        ARCHIVE DESTINATION ${C_DIST_LIB_DIR})

# Install the C binding static library
install(TARGETS compress_utils_c_static
        ARCHIVE DESTINATION ${C_DIST_LIB_DIR})

# Install the C binding headers
install(FILES ${C_BINDING_DIR}/compression_utils.h
        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h
        DESTINATION ${C_DIST_INCLUDE_DIR})

# Copy the README from bindings/c/README.md to the dist/c directory
install(FILES ${C_BINDING_DIR}/README.md
        DESTINATION ${CMAKE_SOURCE_DIR}/dist/c)

######### TESTS #########

# Add tests if the option is enabled
if (ENABLE_TESTS)
    message(STATUS "Tests are enabled. Including the tests directory.")
    add_subdirectory(tests)
else()
    message(STATUS "Tests are disabled. Skipping the tests directory.")
endif()
