## CMake for Python language bindings

######### BINDING SETUP #########

# Locate Python and pybind11
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG QUIET)

# If pybind11 is not found, fetch it
if (NOT pybind11_FOUND)
    message(STATUS "pybind11 not found. Fetching pybind11 from GitHub...")
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.12.1
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# Python binding directories
set(PYTHON_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/python")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PYTHON_BINDING_DIR}/compress_utils_py")

# Define the Python module name
set(PYTHON_MODULE_NAME "compress_utils_py")

# Create the Python module target
pybind11_add_module(${PYTHON_MODULE_NAME} MODULE compression_utils_py.cpp)

# Define the package directory
set(PYTHON_PACKAGE_DIR "${PYTHON_BINDING_DIR}/compress_utils_py")

# Ensure the package directory exists
file(MAKE_DIRECTORY ${PYTHON_PACKAGE_DIR})

# Set the output directory for the module to the package directory
set_target_properties(${PYTHON_MODULE_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${PYTHON_PACKAGE_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${PYTHON_PACKAGE_DIR}
)

# Include directories for the Python module and link to core static lib
target_include_directories(${PYTHON_MODULE_NAME} PRIVATE ${PYTHON_BINDING_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE compress_utils_static)
target_compile_definitions(${PYTHON_MODULE_NAME} PRIVATE ${COMPRESSION_DEFINITIONS} COMPRESSION_UTILS_EXPORT_STATIC)

# Ensure that the Python bindings are built after the core library
add_dependencies(${PYTHON_MODULE_NAME} compress_utils_static)

######### INSTALL #########

if (NOT SCIKIT_BUILD)
    set(PYTHON_DIST_DIR "${CMAKE_SOURCE_DIR}/dist/python")
    message(STATUS "Python distribution directory: ${PYTHON_DIST_DIR}")

    # Install the Python module (compiled .so file) to both destinations
    install(TARGETS ${PYTHON_MODULE_NAME}
        LIBRARY DESTINATION ${PYTHON_DIST_DIR}
        ARCHIVE DESTINATION ${PYTHON_DIST_DIR}
        RUNTIME DESTINATION ${PYTHON_DIST_DIR}
    )

    # Install additional files to both destinations
    foreach(file ${PYTHON_BINDING_DIR}/README.md ${PYTHON_BINDING_DIR}/pyproject.toml ${CMAKE_SOURCE_DIR}/LICENSE ${PYTHON_PACKAGE_DIR}/__init__.py)
        install(FILES ${file}
            DESTINATION ${PYTHON_DIST_DIR}
        )
    endforeach()
else()
    # Fake install README.md and pyproject.toml for scikit-build
    install(FILES ${PYTHON_BINDING_DIR}/README.md ${PYTHON_BINDING_DIR}/pyproject.toml
        DESTINATION ${CMAKE_INSTALL_PREFIX}
    )
endif()

######### TESTS #########

if(ENABLE_TESTS)
    # Add the test with the library path set
    add_test(
        NAME unit_tests_py
        COMMAND ${Python3_EXECUTABLE} ${PYTHON_BINDING_DIR}/tests/test_compression_utils.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(unit_tests_py PROPERTIES ENVIRONMENT "PYTHONPATH=${PYTHON_DIST_DIR}:$ENV{PYTHONPATH}")
endif()