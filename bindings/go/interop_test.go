package compressutils

// Interop tests prove wire-format compatibility with independent
// implementations — not just self-consistency. They mirror the Python
// binding's test_interop.py, but use only Go's standard library so the
// module stays dependency-free and the suite always runs (nothing to skip).
//
// Coverage is limited to the algorithms the stdlib implements:
//
//	gzip  (compress/gzip)  — both directions (RFC 1952)
//	zlib  (compress/zlib)  — both directions (RFC 1950)
//	bzip2 (compress/bzip2) — outbound only; stdlib has no bzip2 encoder
//
// zstd/brotli/lz4/xz/snappy have no stdlib codec; their wire-format interop
// is covered by the Python binding's suite against PyPI references.

import (
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"compress/zlib"
	"io"
	"testing"
)

var interopPayload = bytes.Repeat([]byte("interoperability is the whole point. "), 500)

// outbound: our compressor -> reference decompressor must recover the input.
func decodeWith(t *testing.T, newReader func(io.Reader) (io.Reader, error), c []byte) []byte {
	t.Helper()
	r, err := newReader(bytes.NewReader(c))
	if err != nil {
		t.Fatalf("reference reader: %v", err)
	}
	got, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("reference decode: %v", err)
	}
	return got
}

func TestInteropGzip(t *testing.T) {
	if !Available(Gzip) {
		t.Skip("gzip not available")
	}
	// Outbound: cu.Compress -> stdlib gzip decode.
	c, err := Compress(Gzip, interopPayload, 6)
	if err != nil {
		t.Fatalf("Compress: %v", err)
	}
	got := decodeWith(t, func(r io.Reader) (io.Reader, error) { return gzip.NewReader(r) }, c)
	if !bytes.Equal(got, interopPayload) {
		t.Fatal("outbound: stdlib gzip could not recover our output")
	}

	// Inbound: stdlib gzip encode -> cu.Decompress.
	var buf bytes.Buffer
	gw := gzip.NewWriter(&buf)
	if _, err := gw.Write(interopPayload); err != nil {
		t.Fatalf("stdlib gzip write: %v", err)
	}
	if err := gw.Close(); err != nil {
		t.Fatalf("stdlib gzip close: %v", err)
	}
	dec, err := Decompress(Gzip, buf.Bytes())
	if err != nil {
		t.Fatalf("Decompress of stdlib gzip: %v", err)
	}
	if !bytes.Equal(dec, interopPayload) {
		t.Fatal("inbound: we could not recover stdlib gzip output")
	}
}

func TestInteropZlib(t *testing.T) {
	if !Available(Zlib) {
		t.Skip("zlib not available")
	}
	// Outbound: cu.Compress -> stdlib zlib decode.
	c, err := Compress(Zlib, interopPayload, 6)
	if err != nil {
		t.Fatalf("Compress: %v", err)
	}
	got := decodeWith(t, func(r io.Reader) (io.Reader, error) { return zlib.NewReader(r) }, c)
	if !bytes.Equal(got, interopPayload) {
		t.Fatal("outbound: stdlib zlib could not recover our output")
	}

	// Inbound: stdlib zlib encode -> cu.Decompress.
	var buf bytes.Buffer
	zw := zlib.NewWriter(&buf)
	if _, err := zw.Write(interopPayload); err != nil {
		t.Fatalf("stdlib zlib write: %v", err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("stdlib zlib close: %v", err)
	}
	dec, err := Decompress(Zlib, buf.Bytes())
	if err != nil {
		t.Fatalf("Decompress of stdlib zlib: %v", err)
	}
	if !bytes.Equal(dec, interopPayload) {
		t.Fatal("inbound: we could not recover stdlib zlib output")
	}
}

func TestInteropBzip2(t *testing.T) {
	if !Available(Bz2) {
		t.Skip("bz2 not available")
	}
	// Outbound only: stdlib compress/bzip2 decompresses but cannot encode.
	c, err := Compress(Bz2, interopPayload, 6)
	if err != nil {
		t.Fatalf("Compress: %v", err)
	}
	got := decodeWith(t, func(r io.Reader) (io.Reader, error) { return bzip2.NewReader(r), nil }, c)
	if !bytes.Equal(got, interopPayload) {
		t.Fatal("outbound: stdlib bzip2 could not recover our output")
	}
}
