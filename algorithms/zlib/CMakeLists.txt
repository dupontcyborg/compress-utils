# zlib codec — built from the vendored source tree (third_party/zlib) via the
# manifest. Creates zlib_library, which the gzip codec also links (gzip is zlib
# with a different wire wrapper). See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(zlib)
