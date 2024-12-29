## CMake for Main C++ Tests

message(STATUS "Setting up GoogleTest with Build Type: ${CMAKE_BUILD_TYPE}")

include(ExternalProject)

# Set the base directory for ExternalProject to download external projects
set(EXTERNAL_PROJECTS_DIR ${CMAKE_BINARY_DIR}/external)

# Add an external project to download and build GoogleTest
ExternalProject_Add(
    googletest
    PREFIX ${EXTERNAL_PROJECTS_DIR}/gtest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG origin/main
    GIT_SHALLOW TRUE
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_GMOCK=OFF
      -DBUILD_GTEST=ON
      -DINSTALL_GTEST=OFF
      -Dgtest_force_shared_crt=ON
    LOG_DOWNLOAD OFF
    LOG_UPDATE OFF
    LOG_CONFIGURE OFF
    LOG_BUILD OFF
    LOG_INSTALL OFF
    INSTALL_COMMAND ""
)

# Add the GoogleTest targets to our build
ExternalProject_Get_Property(googletest source_dir binary_dir)

# Create an imported target for gtest
add_library(gtest IMPORTED STATIC GLOBAL)
if (WIN32)
    set_target_properties(gtest PROPERTIES
        IMPORTED_LOCATION ${binary_dir}/lib/Release/${CMAKE_STATIC_LIBRARY_PREFIX}gtest${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
else()
    set_target_properties(gtest PROPERTIES
        IMPORTED_LOCATION ${binary_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}gtest${CMAKE_STATIC_LIBRARY_SUFFIX}
    )
endif()

# Ensure that the gtest targets are built after googletest external project
add_dependencies(gtest googletest)

######### TEST SETUP #########

# Add test executable
file(GLOB TEST_FILES "*.cpp")
add_executable(unit_tests ${TEST_FILES})
add_executable(unit_tests_static ${TEST_FILES})

# Link the test executables to the GoogleTest libraries
add_dependencies(unit_tests compress_utils gtest)
target_link_libraries(unit_tests PRIVATE compress_utils gtest)
add_dependencies(unit_tests_static compress_utils_static gtest)
target_link_libraries(unit_tests_static PRIVATE compress_utils_static gtest)

# Add include directories to both executables
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${source_dir}/googletest/include)
target_include_directories(unit_tests_static PRIVATE ${CMAKE_SOURCE_DIR}/src ${source_dir}/googletest/include)

# Set library input directories of algorithms
set(COMPRESSION_LIBS ${CMAKE_SOURCE_DIR}/algorithms/dist/lib)
target_link_directories(unit_tests PRIVATE ${COMPRESSION_LIBS})
target_link_directories(unit_tests_static PRIVATE ${COMPRESSION_LIBS})

# Define preprocessor definitions
target_compile_definitions(unit_tests PRIVATE COMPRESS_UTILS_EXPORT_SHARED)
target_compile_definitions(unit_tests_static PRIVATE COMPRESS_UTILS_EXPORT_STATIC)

# Include pthreads (on Unix systems)
find_package(Threads REQUIRED)
target_link_libraries(unit_tests PRIVATE Threads::Threads)
target_link_libraries(unit_tests_static PRIVATE Threads::Threads)

# Specify runtime library settings on Windows
if (WIN32)
    target_link_options(unit_tests_static PRIVATE /WHOLEARCHIVE:compress_utils_static)
endif()

# Copy the shared library to the test directory after build
add_custom_command(TARGET unit_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:compress_utils>
        $<TARGET_FILE_DIR:unit_tests>
)

######### TEST REGISTRATION #########

# Enable testing and register both executables
if (ENABLE_TESTS)
    enable_testing()
    add_test(NAME unit_tests COMMAND unit_tests)
    add_test(NAME unit_tests_static COMMAND unit_tests_static)
endif()
