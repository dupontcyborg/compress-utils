## CMake for C++ language bindings (not really bindings, but the main library)

### TODO - make this work to copy/overwrite the algorithms.hpp file

# Set the source path to the original Algorithms.hpp
set(SOURCE_ALGORITHMS_HPP ${CMAKE_SOURCE_DIR}/Algorithms.hpp)

# Set the output path for the processed header
set(OUTPUT_ALGORITHMS_HPP ${CMAKE_BINARY_DIR}/include/Algorithms.hpp)

# Function to process the Algorithms.hpp file and generate a clean version
function(process_algorithms_header)
    # Read the source Algorithms.hpp
    file(READ ${SOURCE_ALGORITHMS_HPP} ALGO_CONTENTS)

    # Initialize an empty string to hold the final contents
    set(FINAL_CONTENTS "")

    # Check if ZLIB is included, and append its enum entry if found
    if (DEFINED INCLUDE_ZLIB)
        string(APPEND FINAL_CONTENTS "    ZLIB,\n")
    endif()

    # Check if ZSTD is included, and append its enum entry if found
    if (DEFINED INCLUDE_ZSTD)
        string(APPEND FINAL_CONTENTS "    ZSTD,\n")
    endif()

    # Replace the #ifdef contents with FINAL_CONTENTS in ALGO_CONTENTS
    string(REGEX REPLACE "#ifdef INCLUDE_[^\\n]*\\n[^\\n]*\\n[^\\n]*endif" "${FINAL_CONTENTS}" ALGO_CONTENTS "${ALGO_CONTENTS}")

    # Write the modified content to the output file
    file(WRITE ${OUTPUT_ALGORITHMS_HPP} "${ALGO_CONTENTS}")
endfunction()

# Call the function to process the header
process_algorithms_header()

# Ensure the output directory exists before installing the header
install(FILES ${OUTPUT_ALGORITHMS_HPP} DESTINATION include)
