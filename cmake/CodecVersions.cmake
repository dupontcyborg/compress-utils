# CodecVersions.cmake — load codec-versions.json and expose <ALGO>_URL /
# <ALGO>_TAG variables for the per-algorithm ExternalProject_Add calls.
#
# Included once from the top-level CMakeLists.txt. Variables propagate to
# every add_subdirectory below it.
#
# JSON is the canonical source — also consumed by tools/sync-codecs.py
# (which translates entries here into bindings/zig/build.zig.zon).

# Anchor on the .cmake file's own location, not CMAKE_SOURCE_DIR — that
# variable points at whichever project root included us, which is the
# repo root for the native build but `bindings/wasm/` for the wasm child.
set(_CU_CODEC_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../codec-versions.json")
if(NOT EXISTS "${_CU_CODEC_MANIFEST}")
    message(FATAL_ERROR "codec-versions.json not found at ${_CU_CODEC_MANIFEST}")
endif()

file(READ "${_CU_CODEC_MANIFEST}" _CU_CODEC_VERSIONS)

foreach(_name zstd brotli zlib bz2 lz4 xz)
    string(JSON _url ERROR_VARIABLE _err GET "${_CU_CODEC_VERSIONS}" ${_name} url)
    if(_err)
        message(FATAL_ERROR "codec-versions.json: missing '${_name}.url' (${_err})")
    endif()
    string(JSON _tag ERROR_VARIABLE _err GET "${_CU_CODEC_VERSIONS}" ${_name} tag)
    if(_err)
        message(FATAL_ERROR "codec-versions.json: missing '${_name}.tag' (${_err})")
    endif()
    string(TOUPPER ${_name} _upper)
    set(${_upper}_URL "${_url}")
    set(${_upper}_TAG "${_tag}")
endforeach()
