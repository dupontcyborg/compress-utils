## C++ binding — header-only INTERFACE library.

add_library(compress_utils_cpp INTERFACE)
target_include_directories(compress_utils_cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(compress_utils_cpp INTERFACE compress_utils)
target_compile_features(compress_utils_cpp INTERFACE cxx_std_20)

if(NOT SCIKIT_BUILD)
    install(FILES include/compress_utils.hpp
            DESTINATION ${CMAKE_SOURCE_DIR}/dist/cpp/include)
endif()

## C++ smoke test for the binding.
if(ENABLE_TESTS)
    add_executable(cpp_smoke_test test/cpp_smoke_test.cpp)
    target_link_libraries(cpp_smoke_test PRIVATE compress_utils_cpp)
    # Static-link the algorithm libs into the test (compress_utils is shared
    # but ships symbols, so the test only needs the C lib + sysdeps).
    if(WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(cpp_smoke_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME cpp_smoke_test COMMAND cpp_smoke_test)
endif()
