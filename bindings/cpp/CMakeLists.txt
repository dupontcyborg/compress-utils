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

## C++ test suite for the binding.
##
## test_compress_utils.cpp exercises the cu:: namespace surface:
##   - free cu::compress / cu::decompress across all algorithms
##   - CompressStream / DecompressStream RAII, including move semantics
##   - cu::Error translation on malformed input (verifies that the .code()
##     accessor is plumbed through from the C status code)
if(ENABLE_TESTS)
    add_executable(test_compress_utils_cpp test/test_compress_utils.cpp)
    target_link_libraries(test_compress_utils_cpp PRIVATE compress_utils_cpp)
    if(WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(test_compress_utils_cpp PRIVATE Threads::Threads)
    endif()
    add_test(NAME test_compress_utils_cpp COMMAND test_compress_utils_cpp)
endif()
