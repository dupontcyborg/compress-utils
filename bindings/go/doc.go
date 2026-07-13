/*
Package compressutils is a Go binding for the compress-utils library: a
unified interface over eight compression algorithms (Zstandard, Brotli,
zlib, gzip, bzip2, LZ4, XZ/LZMA, Snappy), all backed by the same C ABI.

The binding is a thin cgo shim over include/compress_utils.h. Every
algorithm shares one API surface and one level scale (1 fastest .. 10
smallest); the C core maps that scale onto each codec's native range.

# One-shot

	data := []byte("the quick brown fox ...")

	c, err := compressutils.Compress(compressutils.Zstd, data, 5)
	if err != nil {
		// ...
	}
	restored, err := compressutils.Decompress(compressutils.Zstd, c)

Decompress sizes its output from the wire format when the codec records
it (zstd, xz, lz4 frames, snappy); otherwise it transparently falls back
to the streaming decoder, so callers never have to know which is which.

# Streaming

Streaming maps onto the standard io interfaces. Writer is an
io.WriteCloser that compresses into a sink; you must Close it to flush
the final frame. Reader is an io.ReadCloser that decompresses from a
source.

	var buf bytes.Buffer
	w, _ := compressutils.NewWriter(&buf, compressutils.Gzip, 6)
	io.Copy(w, src)
	w.Close() // flushes and finalizes the frame

	r, _ := compressutils.NewReader(&buf, compressutils.Gzip)
	defer r.Close()
	io.Copy(dst, r)

# Linking

This binding links against the compress-utils C shared library. Build it
first from the repository root (this populates dist/c):

	cmake -S . -B build -DBUILD_PYTHON_BINDINGS=OFF -DBUILD_CPP_BINDINGS=OFF
	cmake --build build -j
	cmake --install build

The cgo directives point at dist/c/include and dist/c/lib relative to
this package, and bake an rpath so `go test ./...` works in-tree without
setting LD_LIBRARY_PATH / DYLD_LIBRARY_PATH. See README.md for consuming
the binding from outside the repository.
*/
package compressutils
