# Hand-adapted, decoder-only C implementation; provenance is retained beside it.
# Keep the same object/static facade as the manifest-driven codecs so the
# self-contained archive and WASM build both consume these objects.
set(_CU_LZOKAY_DIR "${CU_REPO_ROOT}/third_party/lzokay")
add_library(lzo_objs OBJECT "${_CU_LZOKAY_DIR}/lzokay.c")
target_include_directories(lzo_objs PUBLIC "${_CU_LZOKAY_DIR}")
set_target_properties(lzo_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(lzo_library STATIC $<TARGET_OBJECTS:lzo_objs>)
target_include_directories(lzo_library PUBLIC "${_CU_LZOKAY_DIR}")
