# snappy codec — built from the vendored source tree (third_party/snappy) via
# the manifest. This is the pure-C port andikleen/snappy-c (a single snappy.c
# plus our portable compat.h shim), NOT google/snappy — so it adds no C++
# runtime dependency. google/snappy (C++) is retained only at
# third_party/snappy-oracle for the differential test (see tests/CMakeLists.txt).
# Creates snappy_library. See cmake/Vendor.cmake and third_party/VENDOR.md.
cu_add_vendored_codec(snappy)
