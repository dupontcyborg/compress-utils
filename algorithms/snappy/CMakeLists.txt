# snappy codec — built from the vendored source tree (third_party/snappy) via
# the manifest. Snappy is C++ (compiled as .cc); consumers link a C++ runtime
# (handled in the root CMakeLists). Our vtable only needs the C API (snappy-c.h).
# Creates snappy_library. See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(snappy)
