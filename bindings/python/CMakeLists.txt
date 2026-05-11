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

# Self-contained wheel: consume the OBJECT library directly. This pulls
# in the C wrapper code as .o files baked into the .pyd/.so, plus the
# algorithm static archives via INTERFACE_LINK_LIBRARIES. No runtime
# dependency on libcompress_utils.{dll,dylib,so} — important for
# `pip install` portability.
#
# We do NOT link `compress_utils_cpp` (the INTERFACE target for the C++
# binding) because its interface link points at the shared library,
# which would silently make this wheel depend on the dylib being on
# disk. Pull in the C++ header by include path instead.
target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE compress_utils_obj)
target_include_directories(${PYTHON_MODULE_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/bindings/cpp/include
)
target_compile_features(${PYTHON_MODULE_NAME} PRIVATE cxx_std_20)

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
