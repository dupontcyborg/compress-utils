## Python binding — pybind11 wrapper over the header-only C++ binding.

find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG QUIET)

if(NOT pybind11_FOUND)
    message(STATUS "pybind11 not found via find_package; fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

set(PYTHON_BINDING_DIR    "${CMAKE_CURRENT_SOURCE_DIR}")
set(PYTHON_PACKAGE_DIR    "${PYTHON_BINDING_DIR}/compress_utils")
set(PYTHON_MODULE_NAME    "compress_utils_py")

pybind11_add_module(${PYTHON_MODULE_NAME} MODULE compress_utils_py.cpp)
target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE compress_utils_cpp)
# Static-link the C core into the Python module so the .so is self-contained.
target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE compress_utils_static)

# Same algorithm-lib link order as the smoke test.
if(INCLUDE_ZSTD)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE zstd_library)
endif()
if(INCLUDE_ZLIB)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE zlib_library)
endif()
if(INCLUDE_BROTLI)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_BZ2)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE bz2_library)
endif()
if(INCLUDE_LZ4)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE lz4_library)
endif()
if(INCLUDE_XZ)
    target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE xz_library)
endif()

set_target_properties(${PYTHON_MODULE_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PYTHON_PACKAGE_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PYTHON_PACKAGE_DIR}
)

# Make sure the compiled module ends up alongside __init__.py.
add_custom_command(TARGET ${PYTHON_MODULE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PYTHON_MODULE_NAME}>
        ${PYTHON_PACKAGE_DIR}
)

######### TESTS #########

if(ENABLE_TESTS)
    add_test(
        NAME test_compress_utils_py
        COMMAND ${Python3_EXECUTABLE} ${PYTHON_BINDING_DIR}/tests/test_compress_utils.py
        WORKING_DIRECTORY ${PYTHON_BINDING_DIR}
    )
    set_tests_properties(test_compress_utils_py PROPERTIES
        ENVIRONMENT "PYTHONPATH=${PYTHON_BINDING_DIR}"
    )
endif()
