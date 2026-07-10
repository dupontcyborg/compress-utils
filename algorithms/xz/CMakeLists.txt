# xz/liblzma codec — built from the vendored source tree (third_party/xz) via
# the manifest. Creates xz_library. LZMA_API_STATIC is PUBLIC so both liblzma's
# own sources and our vtable (which includes <lzma.h>) see the static-API
# declarations. See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(xz PUBLIC_DEFINES LZMA_API_STATIC)
