## CMake for WebAssembly bindings for TypeScript/JavaScript

######### BINDING SETUP #########

# Check for Emscripten
if(NOT DEFINED ENV{EMSDK})
    message(WARNING "EMSDK environment variable not set. WebAssembly bindings may not compile correctly.")
    message(STATUS "Please install and activate Emscripten SDK: https://emscripten.org/docs/getting_started/")
endif()

# WASM binding directories
set(WASM_BINDING_DIR "${CMAKE_SOURCE_DIR}/bindings/wasm")
set(WASM_BUILD_DIR "${WASM_BINDING_DIR}/build")
set(WASM_DIST_DIR "${WASM_BINDING_DIR}/dist")

# Create output directories
file(MAKE_DIRECTORY ${WASM_BUILD_DIR})
file(MAKE_DIRECTORY ${WASM_DIST_DIR})

# Set Emscripten specific flags
# The proper format for Emscripten flags via CMake
set(EM_FLAGS "")
set(EM_FLAGS "${EM_FLAGS} -sWASM=1")
set(EM_FLAGS "${EM_FLAGS} -sEXPORT_ES6=1")
set(EM_FLAGS "${EM_FLAGS} -sMODULARIZE=1")
set(EM_FLAGS "${EM_FLAGS} -sEXPORT_NAME=CompressUtilsFactory")
set(EM_FLAGS "${EM_FLAGS} -sALLOW_MEMORY_GROWTH=1")
set(EM_FLAGS "${EM_FLAGS} -sASSERTIONS=1")
set(EM_FLAGS "${EM_FLAGS} -sNO_EXIT_RUNTIME=1")
set(EM_FLAGS "${EM_FLAGS} --bind")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(EM_FLAGS "${EM_FLAGS} -O3")
endif()

# WASM target
add_executable(compress_utils_wasm ${WASM_BINDING_DIR}/src/compress_utils_wasm.cpp)

# Set output name and location with Emscripten flags
set_target_properties(compress_utils_wasm PROPERTIES
    OUTPUT_NAME "compress_utils"
    RUNTIME_OUTPUT_DIRECTORY "${WASM_BUILD_DIR}"
    LINK_FLAGS "${EM_FLAGS}"
)

# Include directories
target_include_directories(compress_utils_wasm PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/algorithms/dist/include
)

# Link against the static library for smaller output
target_link_libraries(compress_utils_wasm PRIVATE compress_utils_static)
target_compile_definitions(compress_utils_wasm PRIVATE COMPRESS_UTILS_EXPORT_STATIC)

# Ensure the WASM binding is built after the core library
add_dependencies(compress_utils_wasm compress_utils_static)

######### POST BUILD STEPS #########

# Start with empty values
set(ALGORITHM_ENUM_VALUES "")

# Add values conditionally based on which algorithms are enabled
if(INCLUDE_BROTLI)
  set(ALGORITHM_ENUM_VALUES "${ALGORITHM_ENUM_VALUES}    BROTLI = 0,\n")
endif()
if(INCLUDE_XZ)
  set(ALGORITHM_ENUM_VALUES "${ALGORITHM_ENUM_VALUES}    LZMA = 1,\n")
  set(ALGORITHM_ENUM_VALUES "${ALGORITHM_ENUM_VALUES}    XZ = 2,\n")
endif()
if(INCLUDE_ZLIB)
  set(ALGORITHM_ENUM_VALUES "${ALGORITHM_ENUM_VALUES}    ZLIB = 3,\n")
endif()
if(INCLUDE_ZSTD)
  set(ALGORITHM_ENUM_VALUES "${ALGORITHM_ENUM_VALUES}    ZSTD = 4,\n")
endif()

# Configure the TypeScript declaration file
configure_file(
  "${WASM_BINDING_DIR}/src/compress_utils.d.ts.in"
  "${WASM_BUILD_DIR}/compress_utils.d.ts"
  @ONLY
)

# Copy TypeScript interface file and supporting files
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BINDING_DIR}/src/index.ts"
    "${WASM_BUILD_DIR}/index.ts"
    COMMENT "Copying TypeScript interface file"
)

# Copy package.json
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BINDING_DIR}/src/package.json"
    "${WASM_BUILD_DIR}/package.json"
    COMMENT "Copying package.json"
)

# Copy tsconfig.json
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BINDING_DIR}/src/tsconfig.json"
    "${WASM_BUILD_DIR}/tsconfig.json"
    COMMENT "Copying tsconfig.json"
)

# Copy README
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BINDING_DIR}/README.md"
    "${WASM_BUILD_DIR}/README.md"
    COMMENT "Copying README"
)

# Run npm install and build in the build directory
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies and building TypeScript wrapper..."
    COMMAND npm install
    COMMAND npm run build
    WORKING_DIRECTORY ${WASM_BUILD_DIR}
    COMMENT "Building TypeScript wrapper"
)

######### INSTALL #########

# Install to dist directory
install(DIRECTORY ${WASM_BUILD_DIR}/dist/
    DESTINATION ${CMAKE_SOURCE_DIR}/dist/wasm
)

# Also copy the WASM and JS files directly
install(FILES 
    "${WASM_BUILD_DIR}/compress_utils.js"
    "${WASM_BUILD_DIR}/compress_utils.wasm"
    DESTINATION ${CMAKE_SOURCE_DIR}/dist/wasm
)

######### TESTS #########

if(ENABLE_TESTS)
    find_program(NPM_EXECUTABLE npm REQUIRED)
    
    # Create a target to install npm dependencies - make sure this runs explicitly
    add_custom_target(wasm_install_test_dependencies ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies for tests..."
        COMMAND ${NPM_EXECUTABLE} install
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
        COMMENT "Installing WASM test dependencies"
    )
    
    # Add dependency on the main build
    add_dependencies(wasm_install_test_dependencies compress_utils_wasm)
    
    # Copy WASM files to test directory
    add_custom_command(TARGET wasm_install_test_dependencies POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Copying WASM files to test directory..."
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WASM_BUILD_DIR}/compress_utils.js"
        "${WASM_BUILD_DIR}/compress_utils.wasm"
        "${WASM_BINDING_DIR}/tests/"
        COMMENT "Copying WASM files to test directory"
    )
    
    # Create a target that builds the TypeScript tests
    add_custom_target(wasm_build_tests
        COMMAND ${NPM_EXECUTABLE} run test:types
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
        DEPENDS wasm_install_test_dependencies
        COMMENT "Building TypeScript tests"
    )
    
    # Create a custom target to run all tests (not a CTest test)
    add_custom_target(wasm_run_all_tests
        COMMAND ${NPM_EXECUTABLE} test
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
        DEPENDS wasm_build_tests
        COMMENT "Running all WASM tests"
    )
    
    # Add individual tests to CTest
    add_test(
        NAME wasm_unit_tests
        COMMAND ${NPM_EXECUTABLE} run test:unit
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    )
    
    # Browser tests
    add_test(
        NAME wasm_browser_tests
        COMMAND ${NPM_EXECUTABLE} run test:browser
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    )
    
    # ESM module tests
    add_test(
        NAME wasm_esm_tests
        COMMAND ${NPM_EXECUTABLE} run test:esm
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    )
    
    # CommonJS module tests
    add_test(
        NAME wasm_cjs_tests
        COMMAND ${NPM_EXECUTABLE} run test:cjs
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    )
    
    # Set test properties to ensure dependencies are built first
    # and proper environment variables are set
    set_tests_properties(
        wasm_unit_tests
        wasm_browser_tests
        wasm_esm_tests
        wasm_cjs_tests
        PROPERTIES 
        DEPENDS wasm_build_tests
    )
    
    # Add the benchmarks as a custom target (not a test)
    add_custom_target(wasm_benchmarks
        COMMAND ${CMAKE_COMMAND} -E echo "Running WASM compression benchmarks..."
        COMMAND ${NPM_EXECUTABLE} run benchmark || echo "Benchmarks failed"
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
        DEPENDS wasm_install_test_dependencies
        COMMENT "Running WASM compression benchmarks"
    )
endif(ENABLE_TESTS)