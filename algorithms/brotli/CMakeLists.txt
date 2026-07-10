# brotli codec — built from the vendored source tree (third_party/brotli) via
# the manifest. Creates brotli_library (encoder + decoder + common in one
# static lib). See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(brotli)
