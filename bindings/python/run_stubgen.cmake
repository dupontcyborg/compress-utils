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

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OUT_DIR}/compress_utils/compress_utils_py.pyi ${DEST}
    RESULT_VARIABLE _copy_rc
)
if(NOT _copy_rc EQUAL 0)
    message(WARNING "Failed to copy generated .pyi to ${DEST}; keeping committed stub.")
endif()
