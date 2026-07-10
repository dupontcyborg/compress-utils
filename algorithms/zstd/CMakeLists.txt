# zstd codec — built from the vendored source tree (third_party/zstd) via the
# manifest. See cmake/Vendor.cmake and third_party/VENDOR.md. Creates
# zstd_library. No fetch, no configure.
cu_add_vendored_codec(zstd)
