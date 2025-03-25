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
set(WASM_DIST_DIR "${WASM_BUILD_DIR}/dist")

# Create output directories
file(MAKE_DIRECTORY ${WASM_BUILD_DIR})
file(MAKE_DIRECTORY ${WASM_DIST_DIR})

# Set Emscripten specific flags
set(EM_FLAGS "")
set(EM_FLAGS "${EM_FLAGS} -sWASM=1")
set(EM_FLAGS "${EM_FLAGS} -sEXPORT_ES6=1")
set(EM_FLAGS "${EM_FLAGS} -sMODULARIZE=1")
set(EM_FLAGS "${EM_FLAGS} -sEXPORT_NAME=CompressUtilsFactory")
set(EM_FLAGS "${EM_FLAGS} -sALLOW_MEMORY_GROWTH=1")
set(EM_FLAGS "${EM_FLAGS} -sNO_EXIT_RUNTIME=1")
set(EM_FLAGS "${EM_FLAGS} --bind")

# Conditional flags based on build type
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(EM_FLAGS "${EM_FLAGS} -O3")
    set(EM_FLAGS "${EM_FLAGS} -sASSERTIONS=0")  # Disable assertions in release
else()
    set(EM_FLAGS "${EM_FLAGS} -sASSERTIONS=1")  # Keep assertions in debug
endif()

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
set(ALGORITHM_TYPES "")
set(MULTIPLE False)

# Add values conditionally based on which algorithms are enabled
if(INCLUDE_BROTLI)
    set(ALGORITHM_TYPES "${ALGORITHM_TYPES} \"brotli\"")
    set(MULTIPLE True)
endif()
if(INCLUDE_XZ)
    if (MULTIPLE)
        set(ALGORITHM_TYPES "${ALGORITHM_TYPES} | ")
    endif()
    set(ALGORITHM_TYPES "${ALGORITHM_TYPES} \"lzma\" | \"xz\"")
endif()
if(INCLUDE_ZLIB)
    if (MULTIPLE)
        set(ALGORITHM_TYPES "${ALGORITHM_TYPES} | ")
    endif()
    set(ALGORITHM_TYPES "${ALGORITHM_TYPES} \"zlib\"")
endif()
if(INCLUDE_ZSTD)
    if (MULTIPLE)
        set(ALGORITHM_TYPES "${ALGORITHM_TYPES} | ")
    endif()
    set(ALGORITHM_TYPES "${ALGORITHM_TYPES} \"zstd\"")
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

# Copy the WASM and JS files to the dist directory
add_custom_command(TARGET compress_utils_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BUILD_DIR}/compress_utils.js"
    "${WASM_DIST_DIR}/compress_utils.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${WASM_BUILD_DIR}/compress_utils.wasm"
    "${WASM_DIST_DIR}/compress_utils.wasm"
    COMMENT "Copying WASM and JS files to dist directory"
)

######### INSTALL #########

# Install to dist directory
install(DIRECTORY ${WASM_DIST_DIR}
    DESTINATION ${CMAKE_SOURCE_DIR}/dist/wasm
)

######### TESTS #########

if(ENABLE_TESTS)
    find_program(NPM_EXECUTABLE npm REQUIRED)
    
    # Create a target to install npm dependencies - make sure this runs explicitly
    add_custom_target(wasm_install_test_dependencies
        COMMAND ${CMAKE_COMMAND} -E echo "Installing npm dependencies for tests..."
        COMMAND ${NPM_EXECUTABLE} install
        COMMAND ${NPM_EXECUTABLE} run prepare
        WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
        COMMENT "Installing WASM test dependencies"
    )
    
    # Add dependency on the main build
    add_dependencies(wasm_install_test_dependencies compress_utils_wasm)
    
    # # Copy all contents of the WASM build directory to the dist directory
    # add_custom_command(TARGET wasm_install_test_dependencies POST_BUILD
    #     COMMAND ${CMAKE_COMMAND} -E echo "Copying all contents of the WASM build directory to the dist directory..."
    #     COMMAND ${CMAKE_COMMAND} -E copy_directory
    #     "${WASM_BUILD_DIR}"
    #     "${WASM_BINDING_DIR}/dist"
    #     COMMENT "Copying all contents of the WASM build directory to the dist directory"
    # )
    
    # # Create a target that builds the TypeScript tests
    # add_custom_target(wasm_build_tests
    #     COMMAND ${NPM_EXECUTABLE} run test:types
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    #     DEPENDS wasm_install_test_dependencies
    #     COMMENT "Building TypeScript tests"
    # )
    
    # # Create a custom target to run all tests (not a CTest test)
    # add_custom_target(wasm_run_all_tests
    #     COMMAND ${NPM_EXECUTABLE} test
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    #     DEPENDS wasm_build_tests
    #     COMMENT "Running all WASM tests"
    # )
    
    # # Node ESM module tests
    # add_test(
    #     NAME wasm_esm_tests
    #     COMMAND ${NPM_EXECUTABLE} run test:esm
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    # )
    
    # # Node CommonJS module tests
    # add_test(
    #     NAME wasm_cjs_tests
    #     COMMAND ${NPM_EXECUTABLE} run test:cjs
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    # )

    # # Browser tests
    # add_test(
    #     NAME wasm_browser_tests
    #     COMMAND ${NPM_EXECUTABLE} run test:browser
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    # )
    
    # # Set test properties to ensure dependencies are built first
    # # and proper environment variables are set
    # set_tests_properties(
    #     wasm_esm_tests
    #     wasm_cjs_tests
    #     wasm_browser_tests
    #     PROPERTIES 
    #     DEPENDS wasm_build_tests
    # )
    
    # # Add the benchmarks as a custom target (not a test)
    # add_custom_target(wasm_benchmarks
    #     COMMAND ${CMAKE_COMMAND} -E echo "Running WASM compression benchmarks..."
    #     COMMAND ${NPM_EXECUTABLE} run benchmark || echo "Benchmarks failed"
    #     WORKING_DIRECTORY ${WASM_BINDING_DIR}/tests
    #     DEPENDS wasm_install_test_dependencies
    #     COMMENT "Running WASM compression benchmarks"
    # )
    
endif(ENABLE_TESTS)