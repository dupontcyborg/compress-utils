# xz/liblzma codec — built from the vendored source tree (third_party/xz) via
# the manifest. Creates xz_library. LZMA_API_STATIC is PUBLIC so both liblzma's
# own sources and our vtable (which includes <lzma.h>) see the static-API
# declarations. See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(xz PUBLIC_DEFINES LZMA_API_STATIC)

# CRC64 is the .xz check, so every decode runs it. liblzma computes it with
# CLMUL (chosen at run time by CPUID) only when HAVE_USABLE_CLMUL is defined;
# without it the table-driven CRC runs, about 10% of decompress time on x86-64.
# The manifest's define set is portable across targets and this one pulls in x86
# intrinsics, so it is added here, for GCC and Clang targeting x86-64 only.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT MSVC AND NOT EMSCRIPTEN
        AND (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"))
    include(CheckCSourceCompiles)
    check_c_source_compiles("
        #include <immintrin.h>
        __attribute__((__target__(\"ssse3,sse4.1,pclmul\")))
        __m128i f(__m128i a, __m128i b) { return _mm_clmulepi64_si128(a, b, 0); }
        int main(void) { return 0; }" CU_XZ_HAVE_CLMUL)
    if(CU_XZ_HAVE_CLMUL)
        target_compile_definitions(xz_objs PRIVATE HAVE_USABLE_CLMUL)
    endif()
endif()
