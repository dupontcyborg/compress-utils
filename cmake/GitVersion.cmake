# GitVersion.cmake - Extract version from git tags
#
# This module attempts to extract version information from git tags.
# If git is not available or no tags exist, it falls back to the provided default version.
#
# Usage:
#   include(cmake/GitVersion.cmake)
#   get_version_from_git(VERSION_VAR DEFAULT_VERSION)
#
# The version is extracted from tags matching "v*" pattern (e.g., v0.1.0 -> 0.1.0)

function(get_version_from_git OUTPUT_VAR DEFAULT_VERSION)
    # Check if we can use git
    find_package(Git QUIET)

    if(GIT_FOUND)
        # Try to get version from git describe
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match "v*" --abbrev=0
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_RESULT
        )

        if(GIT_RESULT EQUAL 0 AND GIT_TAG)
            # Strip the 'v' prefix from the tag (v0.1.0 -> 0.1.0)
            string(REGEX REPLACE "^v" "" VERSION_FROM_GIT "${GIT_TAG}")

            # Validate it looks like a version (x.y.z)
            if(VERSION_FROM_GIT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
                set(${OUTPUT_VAR} "${VERSION_FROM_GIT}" PARENT_SCOPE)
                message(STATUS "Version from git tag: ${VERSION_FROM_GIT}")
                return()
            endif()
        endif()
    endif()

    # Fall back to default version
    set(${OUTPUT_VAR} "${DEFAULT_VERSION}" PARENT_SCOPE)
    message(STATUS "Using default version: ${DEFAULT_VERSION}")
endfunction()
