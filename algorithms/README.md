# compress-utils/algorithms

This folder is used during the `compress-utils` build process to:

- Clone upstream compression algorithms into `algorithms/<algorithm>/src/`
- Build these algorithms into `algorithms/<algorithm>/build/`
- Copy their headers into `algorithms/<algorithm>/dist/include/`
- Copy their static lib into `algorithms/<algorithm>/dist/lib/`

The entire process is orchestrated by CMake, with a top-level `CMakeLists.txt` in the root of the project, and a child `CMakeLists.txt` for each algorithm in `algorithms/<algorithm>/` responsible for cloning and building each compression algorithm.