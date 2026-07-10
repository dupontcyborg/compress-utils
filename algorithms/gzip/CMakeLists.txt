# gzip codec — not a separate upstream. It's the same zlib library as the
# `zlib` codec, driven with a different windowBits (RFC 1952 wrapper), so it
# builds from the vendored zlib sources (third_party/zlib) via the manifest.
#
# The native build does NOT add this subdirectory — it links the zlib codec's
# `zlib_library` directly for gzip (see the root CMakeLists), so zlib is built
# once. This file exists for the isolated WASM per-algorithm build, which
# configures one codec at a time and expects a `gzip_library` target.
cu_add_vendored_codec(gzip MANIFEST_KEY zlib)
