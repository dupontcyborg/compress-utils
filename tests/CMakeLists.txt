## Phase 1 test setup — minimal C smoke tests for the new C ABI.
##
## The old GoogleTest-based C++ test suite covered the old C++ API which
## no longer exists. Those tests will be ported (or replaced) in Phase 4
## alongside the C++ binding. Until then, smoke_test.c gives us a
## buildable, runnable check that the C core works end-to-end.

add_executable(smoke_test smoke_test.c)
target_link_libraries(smoke_test PRIVATE compress_utils_static)
target_include_directories(smoke_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Static link of the algorithm libs needs explicit link order.
if(INCLUDE_ZSTD)
    target_link_libraries(smoke_test PRIVATE zstd_library)
endif()
if(INCLUDE_ZLIB)
    target_link_libraries(smoke_test PRIVATE zlib_library)
endif()
if(INCLUDE_BROTLI)
    target_link_libraries(smoke_test PRIVATE brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_BZ2)
    target_link_libraries(smoke_test PRIVATE bz2_library)
endif()
if(INCLUDE_LZ4)
    target_link_libraries(smoke_test PRIVATE lz4_library)
endif()
if(INCLUDE_XZ)
    target_link_libraries(smoke_test PRIVATE xz_library)
endif()

if(WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(smoke_test PRIVATE Threads::Threads)
endif()

if(ENABLE_TESTS)
    add_test(NAME smoke_test COMMAND smoke_test)
endif()
