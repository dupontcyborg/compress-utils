## CMake for C language bindings

######### BINDING SETUP #########

# Set C standard and inherit parent flags for optimization
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

# C binding directories
set(C_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/c")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${C_BINDING_DIR}/build")

# Create shared and static library targets
add_library(compress_utils_c SHARED compress_utils_c.cpp)
add_library(compress_utils_c_static STATIC compress_utils_c.cpp)

# Include directories for both libraries and link to core static lib
target_include_directories(compress_utils_c PRIVATE ${C_BINDING_DIR})
target_include_directories(compress_utils_c_static PRIVATE ${C_BINDING_DIR})
if (WIN32)
    target_link_libraries(compress_utils_c PRIVATE compress_utils)
    target_compile_definitions(compress_utils_c PRIVATE COMPRESS_UTILS_C_EXPORT_SHARED)
else()
    target_link_libraries(compress_utils_c PRIVATE compress_utils_static)
    target_compile_definitions(compress_utils_c PRIVATE COMPRESS_UTILS_C_EXPORT_SHARED COMPRESS_UTILS_EXPORT_STATIC)
endif()
target_compile_definitions(compress_utils_c_static PRIVATE COMPRESS_UTILS_C_EXPORT_STATIC COMPRESS_UTILS_EXPORT_STATIC)
target_link_libraries(compress_utils_c_static PRIVATE compress_utils_static)

# Ensure that the C bindings are built after the core library
add_dependencies(compress_utils_c compress_utils)
add_dependencies(compress_utils_c compress_utils_static)
add_dependencies(compress_utils_c_static compress_utils_static)

######### INSTALL #########

# Install directories
set(C_DIST_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/c/lib")
set(C_DIST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dist/c/include/compress_utils_c")
set(C_SHARED_DIR "${CMAKE_SOURCE_DIR}/dist/c/shared")
set(C_STATIC_DIR "${CMAKE_SOURCE_DIR}/dist/c/static")

# Configure the header file
configure_file(
    ${C_BINDING_DIR}/algorithms.h.in
    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h
    @ONLY
)

# Install libraries (shared and static) and headers
install(TARGETS compress_utils_c
    LIBRARY DESTINATION ${C_SHARED_DIR}/lib
    ARCHIVE DESTINATION ${C_SHARED_DIR}/lib
    RUNTIME DESTINATION ${C_SHARED_DIR}/lib
)
install(TARGETS compress_utils_c_static
    LIBRARY DESTINATION ${C_STATIC_DIR}/lib
    ARCHIVE DESTINATION ${C_STATIC_DIR}/lib
)

# Install headers
file(GLOB HEADERS ${C_BINDING_DIR}/compress_utils.h
                  ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/algorithms.h)
install(FILES ${HEADERS}
        DESTINATION ${C_SHARED_DIR}/include)
install(FILES ${HEADERS}
        DESTINATION ${C_STATIC_DIR}/include)

# Install and rename the correct symbol_exports headers
install (FILES ${C_BINDING_DIR}/symbol_exports_shared.h
         DESTINATION ${C_SHARED_DIR}/include
            RENAME symbol_exports.h)
install (FILES ${C_BINDING_DIR}/symbol_exports_static.h
            DESTINATION ${C_STATIC_DIR}/include
                RENAME symbol_exports.h)

# Install README
install(FILES ${C_BINDING_DIR}/README.md DESTINATION ${CMAKE_SOURCE_DIR}/dist/c)

# Optionally install static dependencies if building static
if (TARGET compress_utils_c_static AND NOT WIN32)
    file(GLOB TARGET_LIBS ${ALGORITHMS_LIB_DIR}/*)
    install(FILES ${TARGET_LIBS}
            DESTINATION ${C_STATIC_DIR}/lib)
endif()

######### WINDOWS LINKING #########

# Set runtime library flags for Windows targets
if (WIN32)
    add_custom_command(TARGET compress_utils_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:compress_utils>
        $<TARGET_FILE_DIR:compress_utils_c>
    )
endif()

######### TESTS #########

# Optionally add tests
if (ENABLE_TESTS)
    add_subdirectory(tests)
    add_dependencies(unit_tests_c compress_utils_c)
    add_dependencies(unit_tests_c_static compress_utils_c_static)
endif()