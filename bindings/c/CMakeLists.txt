## CMake for C language bindings

######### BINDING SETUP #########

# Set C standard and inherit parent flags for optimization
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

# C binding directories
set(C_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/c")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${C_BINDING_DIR}/build")

# Create shared and static library targets
add_library(compress_utils_c SHARED compression_utils_c.cpp)
add_library(compress_utils_c_static STATIC compression_utils_c.cpp)

# Include directories for both libraries and link to core static lib
target_include_directories(compress_utils_c PRIVATE ${C_BINDING_DIR})
target_include_directories(compress_utils_c_static PRIVATE ${C_BINDING_DIR})
target_link_libraries(compress_utils_c PRIVATE compress_utils_static)
target_link_libraries(compress_utils_c_static PRIVATE compress_utils_static)

######### INSTALL #########

# Install directories
set(C_DIST_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/c/lib")
set(C_DIST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dist/c/include/compression_utils_c")

# Configure the header file
configure_file(
    ${C_BINDING_DIR}/algorithms.h.in
    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h
    @ONLY
)

# Install libraries (shared and static) and headers
install(TARGETS compress_utils_c compress_utils_c_static
        LIBRARY DESTINATION ${C_DIST_LIB_DIR}
        ARCHIVE DESTINATION ${C_DIST_LIB_DIR}/static)

install(FILES ${C_BINDING_DIR}/compression_utils.h
              ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h
        DESTINATION ${C_DIST_INCLUDE_DIR})

# Optionally install static dependencies if building static
if (TARGET compress_utils_c_static)
    install(FILES ${TARGET_LIBS} DESTINATION ${C_DIST_LIB_DIR}/static)
endif()

# Install README
install(FILES ${C_BINDING_DIR}/README.md DESTINATION ${CMAKE_SOURCE_DIR}/dist/c)

######### TESTS #########

# Optionally add tests
if (ENABLE_TESTS)
    add_subdirectory(tests)
    add_dependencies(unit_tests_c compress_utils_c)
    add_dependencies(unit_tests_c_static compress_utils_c_static)
endif()