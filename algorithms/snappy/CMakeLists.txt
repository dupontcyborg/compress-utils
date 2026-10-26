# snappy codec — first-party C core (src/algorithms/snappy/core), not a vendored
# upstream: a self-contained compressor/decompressor with no dependency on the
# compress-utils ABI, adapted to it by src/algorithms/snappy/snappy.c. Same
# object/static facade as the manifest-driven codecs so the self-contained
# archive and the WASM build consume these objects. Creates snappy_library.
# google/snappy (C++) lives only at third_party/snappy-oracle, for the
# differential test (see tests/CMakeLists.txt).
set(_CU_SNAPPY_CORE_DIR "${CU_REPO_ROOT}/src/algorithms/snappy/core")
add_library(snappy_objs OBJECT "${_CU_SNAPPY_CORE_DIR}/snappy_core.c")
target_include_directories(snappy_objs PUBLIC "${_CU_SNAPPY_CORE_DIR}")
set_target_properties(snappy_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(snappy_library STATIC $<TARGET_OBJECTS:snappy_objs>)
target_include_directories(snappy_library PUBLIC "${_CU_SNAPPY_CORE_DIR}")
set_target_properties(snappy_library PROPERTIES POSITION_INDEPENDENT_CODE ON)
