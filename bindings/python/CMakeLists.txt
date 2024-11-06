## CMake for Python language bindings

######### BINDING SETUP #########

# Locate Python and pybind11
find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

# Python binding directories
set(PYTHON_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/python")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PYTHON_BINDING_DIR}/build")

# Define the Python module name
set(PYTHON_MODULE_NAME "compress_utils_py")

# Create the Python module target
pybind11_add_module(${PYTHON_MODULE_NAME} MODULE compression_utils_py.cpp)

# Include directories for the Python module and link to core static lib
target_include_directories(${PYTHON_MODULE_NAME} PRIVATE ${PYTHON_BINDING_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(${PYTHON_MODULE_NAME} PRIVATE compress_utils_static)
target_compile_definitions(${PYTHON_MODULE_NAME} PRIVATE ${COMPRESSION_DEFINITIONS} COMPRESSION_UTILS_EXPORT_STATIC)

# Ensure that the Python bindings are built after the core library
add_dependencies(${PYTHON_MODULE_NAME} compress_utils_static)

######### INSTALL #########

# Install directories
set(PYTHON_DIST_DIR "${CMAKE_SOURCE_DIR}/dist/python")

# Install the Python module
install(TARGETS ${PYTHON_MODULE_NAME}
    LIBRARY DESTINATION ${PYTHON_DIST_DIR}
    ARCHIVE DESTINATION ${PYTHON_DIST_DIR}
    RUNTIME DESTINATION ${PYTHON_DIST_DIR}
)

