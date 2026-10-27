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
#
# Source patches: third_party/<codec>/ is verbatim upstream. A build that sets
# CU_VENDOR_APPLY_PATCHES (the WASM build) compiles a patched build-directory
# copy of it instead, made by applying third_party/patches/<codec>/*.patch in
# sorted order. Every other build never sees a patch. See third_party/VENDOR.md.

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

# Stage a patched copy of third_party/<key>/ under the build tree and return its
# path in <out_var>. Re-staged only when the manifest tag/tree hash or a patch
# changes. A patch that fails to apply, or applies without changing a file it
# names, is a configure error naming the codec, its pinned tag and the patch.
function(_cu_stage_patched_codec out_var key tag tree_hash)
    find_program(CU_GIT_EXECUTABLE git)
    if(NOT CU_GIT_EXECUTABLE)
        message(FATAL_ERROR "git is required to apply the WASM patches for '${key}' "
                            "(third_party/patches/${key}) but was not found on PATH")
    endif()

    set(_src "${CU_VENDOR_DIR}/${key}")
    set(_root "${CMAKE_BINARY_DIR}/vendor-patched")
    set(_stage "${_root}/${key}")
    set(_stamp "${_root}/${key}.stamp")

    set(_want "${tag} ${tree_hash}")
    foreach(_p IN LISTS ARGN)
        file(SHA256 "${_p}" _h)
        get_filename_component(_n "${_p}" NAME)
        string(APPEND _want "\n${_n} ${_h}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ARGN})

    set(_have "")
    if(EXISTS "${_stamp}" AND EXISTS "${_stage}")
        file(READ "${_stamp}" _have)
    endif()
    if(NOT _have STREQUAL _want)
        file(REMOVE "${_stamp}")
        file(REMOVE_RECURSE "${_stage}")
        file(MAKE_DIRECTORY "${_root}")
        file(COPY "${_src}" DESTINATION "${_root}")

        foreach(_p IN LISTS ARGN)
            get_filename_component(_n "${_p}" NAME)
            # Files the patch names ("+++ b/<path>"), hashed before applying so
            # we can tell a patch that changed nothing from one that applied.
            file(STRINGS "${_p}" _targets REGEX "^\\+\\+\\+ b/")
            list(TRANSFORM _targets REPLACE "^\\+\\+\\+ b/" "")
            foreach(_t IN LISTS _targets)
                set(_before_${_t} "absent")
                if(EXISTS "${_stage}/${_t}")
                    file(SHA256 "${_stage}/${_t}" _before_${_t})
                endif()
            endforeach()
            # GIT_DIR pointing nowhere makes `git apply` act as plain `patch -p1`
            # in the working directory. Inside a repository (the build tree
            # usually is) it would silently skip paths that do not exist
            # relative to the repository root.
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E env "GIT_DIR=${_root}/no-git"
                        "${CU_GIT_EXECUTABLE}" apply -p1 "${_p}"
                WORKING_DIRECTORY "${_stage}"
                RESULT_VARIABLE _rc
                ERROR_VARIABLE _err)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR
                    "WASM patch ${_n} does not apply to ${key} ${tag}:\n${_err}\n"
                    "Either ${key} was re-vendored or the patch was edited. Fix the "
                    "patch by hand (see third_party/VENDOR.md, \"Patches for the "
                    "WASM build\").")
            endif()
            foreach(_t IN LISTS _targets)
                set(_after "absent")
                if(EXISTS "${_stage}/${_t}")
                    file(SHA256 "${_stage}/${_t}" _after)
                endif()
                if(_after STREQUAL _before_${_t})
                    message(FATAL_ERROR
                        "WASM patch ${_n} for ${key} ${tag} did not change ${_t}")
                endif()
            endforeach()
        endforeach()
        file(WRITE "${_stamp}" "${_want}")
    endif()
    set(${out_var} "${_stage}" PARENT_SCOPE)
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
    if(CU_VENDOR_APPLY_PATCHES)
        file(GLOB _patches "${CU_VENDOR_DIR}/patches/${_key}/*.patch")
        list(SORT _patches)
        if(_patches)
            string(JSON _tag GET "${_manifest}" codecs "${_key}" tag)
            string(JSON _tree GET "${_manifest}" codecs "${_key}" tree_sha256)
            _cu_stage_patched_codec(_codec_dir "${_key}" "${_tag}" "${_tree}" ${_patches})
        endif()
    endif()
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
