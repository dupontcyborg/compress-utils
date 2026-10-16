# run_stubgen.cmake — regenerate the .pyi stub for the built pybind11 module.
#
# Invoked as a POST_BUILD step via `cmake -P`. Best-effort by design: the
# committed compress_utils_py.pyi is the source of truth, so a stubgen failure
# warns but never fails the build (and thus never breaks a `pip install`/wheel).
#
# Expects -D: PYEXE, BINDING_DIR, MODULE, OUT_DIR, DEST
#
# Why this script exists instead of an inline `cmake -E env PYTHONPATH=...`:
# under pip's build isolation the build dependencies (including
# pybind11-stubgen) live on PYTHONPATH as an overlay. Setting PYTHONPATH to
# just BINDING_DIR clobbers that overlay, so `python -m pybind11_stubgen`
# can no longer import itself ("No module named pybind11_stubgen"). We
# PREPEND BINDING_DIR to the existing PYTHONPATH instead of replacing it, so
# both the freshly built module and the build-env packages stay importable.

if(WIN32)
    set(_sep ";")
else()
    set(_sep ":")
endif()

set(_existing "$ENV{PYTHONPATH}")
if(_existing STREQUAL "")
    set(ENV{PYTHONPATH} "${BINDING_DIR}")
else()
    set(ENV{PYTHONPATH} "${BINDING_DIR}${_sep}${_existing}")
endif()

execute_process(
    COMMAND ${PYEXE} -m pybind11_stubgen ${MODULE} -o ${OUT_DIR}
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    message(WARNING
        "pybind11-stubgen failed (rc=${_rc}); keeping the committed .pyi. "
        "Stubs may be stale until it runs successfully.")
    return()
endif()

# pybind11 3 types buffer arguments as collections.abc.Buffer, which only
# exists on Python 3.12+. typing_extensions.Buffer works on every version we
# support (type checkers bundle typing_extensions), so use it instead.
set(_stub ${OUT_DIR}/compress_utils/compress_utils_py.pyi)
file(READ ${_stub} _text)
string(REPLACE "collections.abc.Buffer" "typing_extensions.Buffer" _text "${_text}")
if(NOT _text MATCHES "collections\\.abc\\.")
    string(REPLACE "import collections.abc\n" "" _text "${_text}")
endif()
if(_text MATCHES "typing_extensions\\." AND NOT _text MATCHES "import typing_extensions")
    string(REPLACE "import typing\n" "import typing\nimport typing_extensions\n" _text "${_text}")
endif()
file(WRITE ${_stub} "${_text}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_stub} ${DEST}
    RESULT_VARIABLE _copy_rc
)
if(NOT _copy_rc EQUAL 0)
    message(WARNING "Failed to copy generated .pyi to ${DEST}; keeping committed stub.")
endif()
