## CMake for Main C++ Tests

message(STATUS "Setting up GoogleTest with Build Type: ${CMAKE_BUILD_TYPE}")

include(FetchContent)

# Set the base directory for FetchContent to clone external projects
set(FETCHCONTENT_BASE_DIR ${CMAKE_BINARY_DIR}/external)

# Fetch GoogleTest into build/external/googletest
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG origin/main
  SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/googletest
)

# Prevent GoogleTest from automatically adding its targets
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "Disable build of googlemock" FORCE)
set(BUILD_GTEST OFF CACHE BOOL "Disable build of googletest" FORCE)

FetchContent_MakeAvailable(googletest)

######### GTEST TARGETS #########

# Build GoogleTest with dynamic runtime (/MD)
add_library(gtest_md STATIC
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc)
add_library(gtest_main_md STATIC
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc)
target_include_directories(gtest_md PUBLIC
    ${googletest_SOURCE_DIR}/googletest
    ${googletest_SOURCE_DIR}/googletest/include)
target_include_directories(gtest_main_md PUBLIC
    ${googletest_SOURCE_DIR}/googletest
    ${googletest_SOURCE_DIR}/googletest/include)
target_link_libraries(gtest_main_md PUBLIC gtest_md)
# set_target_properties(gtest_md PROPERTIES
#     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# set_target_properties(gtest_main_md PROPERTIES
#     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Build GoogleTest with static runtime (/MT)
add_library(gtest_mt STATIC
    ${googletest_SOURCE_DIR}/googletest/src/gtest-all.cc)
add_library(gtest_main_mt STATIC
    ${googletest_SOURCE_DIR}/googletest/src/gtest_main.cc)
target_include_directories(gtest_mt PUBLIC
    ${googletest_SOURCE_DIR}/googletest
    ${googletest_SOURCE_DIR}/googletest/include)
target_include_directories(gtest_main_mt PUBLIC
    ${googletest_SOURCE_DIR}/googletest
    ${googletest_SOURCE_DIR}/googletest/include)
target_link_libraries(gtest_main_mt PUBLIC gtest_mt)
# set_target_properties(gtest_mt PROPERTIES
#     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# set_target_properties(gtest_main_mt PROPERTIES
#     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

######### TEST SETUP #########

# Add test executables
file(GLOB TEST_FILES "*.cpp")
add_executable(unit_tests ${TEST_FILES})
add_executable(unit_tests_static ${TEST_FILES})

# Link dynamic libraries for `unit_tests`
add_dependencies(unit_tests compress_utils)
target_link_libraries(unit_tests PRIVATE compress_utils gtest_main_md)

# Link static libraries for `unit_tests_static`
add_dependencies(unit_tests_static compress_utils_static)
target_link_libraries(unit_tests_static PRIVATE compress_utils_static gtest_main_mt)

# Add include directories to both executables
set(GTEST_INCLUDE_DIRS ${googletest_SOURCE_DIR}/googletest/include)
set(COMMON_INCLUDES ${CMAKE_SOURCE_DIR}/src ${GTEST_INCLUDE_DIRS})
target_include_directories(unit_tests PRIVATE ${COMMON_INCLUDES})
target_include_directories(unit_tests_static PRIVATE ${COMMON_INCLUDES})

# Set library input directories of algorithms
set(COMPRESSION_LIBS ${CMAKE_SOURCE_DIR}/algorithms/dist/lib)
target_link_directories(unit_tests PRIVATE ${COMPRESSION_LIBS})
target_link_directories(unit_tests_static PRIVATE ${COMPRESSION_LIBS})

target_compile_definitions(unit_tests PRIVATE COMPRESSION_UTILS_EXPORT_SHARED)
target_compile_definitions(unit_tests_static PRIVATE COMPRESSION_UTILS_EXPORT_STATIC)

# Include pthreads
find_package(Threads REQUIRED)
target_link_libraries(unit_tests PRIVATE Threads::Threads)
target_link_libraries(unit_tests_static PRIVATE Threads::Threads)

# Specify runtime library settings on Windows
if (WIN32)
    # set_target_properties(unit_tests PROPERTIES
    #     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    # )
    # set_target_properties(unit_tests_static PROPERTIES
    #     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    # )
    target_link_options(unit_tests_static PRIVATE /WHOLEARCHIVE:compress_utils_static)
endif()

# Copy the shared library to the test directory
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
