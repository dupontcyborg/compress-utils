# Vendor.cmake — build a codec static library from the vendored source tree.
#
# Replaces the old per-codec ExternalProject_Add (git fetch + sub-configure)
# with a plain add_library() over the curated sources in third_party/, driven
# by third_party/manifest.json (produced by tools/vendor-codecs.py). No network,
# no configure step: the manifest already encodes sources, include dirs, and the
# portable -D define set for every target.
#
# cu_add_vendored_codec(<name>) creates an OBJECT library <name>_objs (the
# compiled objects) and a STATIC facade <name>_library over it, with include
# dirs PUBLIC (so consumers that #include the codec's headers pick them up) and
# build-time defines PRIVATE. Header-visible defines that our vtable also needs
# (e.g. LZMA_API_STATIC) are added PUBLIC via the optional PUBLIC_DEFINES
# argument. The <name>_objs target lets compress_utils_static bundle every
# codec's objects into one self-contained archive (see BUILD_STATIC_LIB).

# Repo root holding third_party/ — the WASM per-algo child build sets
# CU_REPO_ROOT since its CMAKE_SOURCE_DIR is the wasm project, not the repo.
if(NOT DEFINED CU_REPO_ROOT)
    set(CU_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(CU_VENDOR_DIR "${CU_REPO_ROOT}/third_party")
set(CU_VENDOR_MANIFEST "${CU_VENDOR_DIR}/manifest.json")

# Read a JSON string array at codecs.<name>.<key> into <out_var> as a list.
function(_cu_json_array out_var manifest name key)
    string(JSON _arr ERROR_VARIABLE _err GET "${manifest}" codecs "${name}" "${key}")
    set(_result "")
    if(_arr AND NOT _err)
        string(JSON _n LENGTH "${_arr}")
        if(_n GREATER 0)
            math(EXPR _last "${_n} - 1")
            foreach(_i RANGE ${_last})
                string(JSON _v GET "${_arr}" ${_i})
                list(APPEND _result "${_v}")
            endforeach()
        endif()
    endif()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

# cu_add_vendored_codec(<name> [MANIFEST_KEY <key>] [PUBLIC_DEFINES ...])
#   <name>        target created is <name>_library
#   MANIFEST_KEY  manifest codec to source from (defaults to <name>); lets gzip
#                 build a gzip_library from the vendored zlib sources
function(cu_add_vendored_codec name)
    cmake_parse_arguments(ARG "" "MANIFEST_KEY" "PUBLIC_DEFINES" ${ARGN})
    set(_key "${name}")
    if(ARG_MANIFEST_KEY)
        set(_key "${ARG_MANIFEST_KEY}")
    endif()

    if(NOT EXISTS "${CU_VENDOR_MANIFEST}")
        message(FATAL_ERROR "vendored manifest not found: ${CU_VENDOR_MANIFEST}\n"
                            "run tools/vendor-codecs.py to regenerate third_party/")
    endif()
    file(READ "${CU_VENDOR_MANIFEST}" _manifest)

    set(_codec_dir "${CU_VENDOR_DIR}/${_key}")
    _cu_json_array(_sources "${_manifest}" "${_key}" "sources")
    _cu_json_array(_includes "${_manifest}" "${_key}" "include_dirs")
    _cu_json_array(_defines "${_manifest}" "${_key}" "defines")

    if(NOT _sources)
        message(FATAL_ERROR "codec '${name}' has no sources in ${CU_VENDOR_MANIFEST}")
    endif()

    list(TRANSFORM _sources PREPEND "${_codec_dir}/")
    list(TRANSFORM _includes PREPEND "${_codec_dir}/")

    # The codec's objects live in an OBJECT library (<name>_objs) so a bundled
    # self-contained archive (compress_utils_static) can pull them in via
    # $<TARGET_OBJECTS:...>. The STATIC <name>_library is derived from those
    # objects and is what all existing consumers (native lib, WASM, tests) link;
    # its name and behaviour are unchanged.
    add_library(${name}_objs OBJECT ${_sources})
    target_include_directories(${name}_objs PUBLIC ${_includes})
    target_compile_definitions(${name}_objs
        PRIVATE ${_defines}
        PUBLIC ${ARG_PUBLIC_DEFINES})
    set_target_properties(${name}_objs PROPERTIES
        POSITION_INDEPENDENT_CODE ON)

    # Silence upstream warnings — we don't own this code and build with -Werror
    # nowhere in the codec libs. Keeps a clean build log across 8 upstreams.
    if(MSVC)
        target_compile_options(${name}_objs PRIVATE /w)
    else()
        target_compile_options(${name}_objs PRIVATE -w)
    endif()

    # STATIC facade over the objects. Mirror the OBJECT library's PUBLIC usage
    # requirements (include dirs + header-visible defines) onto it so consumers
    # that link <name>_library keep compiling exactly as before. Build-time
    # PRIVATE defines aren't needed here — no sources compile at this target.
    add_library(${name}_library STATIC $<TARGET_OBJECTS:${name}_objs>)
    target_include_directories(${name}_library PUBLIC ${_includes})
    target_compile_definitions(${name}_library PUBLIC ${ARG_PUBLIC_DEFINES})
    set_target_properties(${name}_library PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
endfunction()
