package compressutils

import (
	"bytes"
	"errors"
	"io"
	"testing"
)

// allAlgos is every algorithm the binding knows about. Tests skip any that
// the linked library was not compiled with.
var allAlgos = []Algorithm{
	Zstd, Brotli, Zlib, Bz2, Lz4, Xz, Snappy, Gzip, Deflate,
}

// payloads spans the edge cases the C interop suite also covers: empty,
// tiny, repetitive-and-compressible, and incompressible/binary.
func payloads() map[string][]byte {
	text := bytes.Repeat([]byte("The quick brown fox jumps over the lazy dog. "), 400)
	binary := make([]byte, 32*1024)
	for i := range binary {
		binary[i] = byte((i*13 + 7) & 0xff)
	}
	return map[string][]byte{
		"empty":     {},
		"one_byte":  {'A'},
		"text_18k":  text,
		"binary32k": binary,
	}
}

func TestVersion(t *testing.T) {
	if v := Version(); v == "" {
		t.Fatal("Version() returned empty string")
	}
}

func TestAvailableAndName(t *testing.T) {
	available := 0
	for _, a := range allAlgos {
		if Available(a) {
			available++
			if a.Name() == "" {
				t.Errorf("available algorithm %d has no name", int(a))
			}
		}
	}
	if available == 0 {
		t.Fatal("no algorithms available in this build")
	}
}

func TestOneShotRoundTrip(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		for name, data := range payloads() {
			t.Run(a.Name()+"/"+name, func(t *testing.T) {
				c, err := Compress(a, data, DefaultLevel)
				if err != nil {
					t.Fatalf("Compress: %v", err)
				}
				got, err := Decompress(a, c)
				if err != nil {
					t.Fatalf("Decompress: %v", err)
				}
				if !bytes.Equal(got, data) {
					t.Fatalf("round-trip mismatch: got %d bytes, want %d", len(got), len(data))
				}
			})
		}
	}
}

func TestStreamingRoundTrip(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		data := payloads()["binary32k"]
		t.Run(a.Name(), func(t *testing.T) {
			var buf bytes.Buffer
			w, err := NewWriter(&buf, a, DefaultLevel)
			if err != nil {
				t.Fatalf("NewWriter: %v", err)
			}
			// Feed in three uneven chunks to exercise the drain protocol.
			for _, chunk := range [][]byte{data[:5000], data[5000:20000], data[20000:]} {
				if _, err := w.Write(chunk); err != nil {
					t.Fatalf("Write: %v", err)
				}
			}
			if err := w.Close(); err != nil {
				t.Fatalf("Close: %v", err)
			}

			r, err := NewReader(bytes.NewReader(buf.Bytes()), a)
			if err != nil {
				t.Fatalf("NewReader: %v", err)
			}
			defer r.Close()
			got, err := io.ReadAll(r)
			if err != nil {
				t.Fatalf("ReadAll: %v", err)
			}
			if !bytes.Equal(got, data) {
				t.Fatalf("stream round-trip mismatch: got %d bytes, want %d", len(got), len(data))
			}
		})
	}
}

// TestCrossAPI proves the one-shot and streaming paths produce byte-compatible
// wire formats: stream output must decode one-shot, and vice versa. This is
// the class of test that catches framing mismatches between the two paths.
func TestCrossAPI(t *testing.T) {
	data := payloads()["binary32k"]
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			// stream-compress -> one-shot decompress
			var buf bytes.Buffer
			w, err := NewWriter(&buf, a, DefaultLevel)
			if err != nil {
				t.Fatalf("NewWriter: %v", err)
			}
			if _, err := w.Write(data); err != nil {
				t.Fatalf("Write: %v", err)
			}
			if err := w.Close(); err != nil {
				t.Fatalf("Close: %v", err)
			}
			got, err := Decompress(a, buf.Bytes())
			if err != nil {
				t.Fatalf("one-shot Decompress of streamed output: %v", err)
			}
			if !bytes.Equal(got, data) {
				t.Fatal("stream->one-shot mismatch")
			}

			// one-shot compress -> stream decompress
			c, err := Compress(a, data, DefaultLevel)
			if err != nil {
				t.Fatalf("Compress: %v", err)
			}
			r, err := NewReader(bytes.NewReader(c), a)
			if err != nil {
				t.Fatalf("NewReader: %v", err)
			}
			defer r.Close()
			got2, err := io.ReadAll(r)
			if err != nil {
				t.Fatalf("stream Read of one-shot output: %v", err)
			}
			if !bytes.Equal(got2, data) {
				t.Fatal("one-shot->stream mismatch")
			}
		})
	}
}

func TestDecompressGarbage(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			// Random-looking bytes should be rejected, not panic or hang.
			garbage := bytes.Repeat([]byte{0xde, 0xad, 0xbe, 0xef}, 64)
			if _, err := Decompress(a, garbage); err == nil {
				t.Fatal("expected error decompressing garbage, got nil")
			}
		})
	}
}

func TestErrorType(t *testing.T) {
	if !Available(Zstd) {
		t.Skip("zstd not available")
	}
	_, err := Decompress(Zstd, []byte{0x00, 0x01, 0x02, 0x03})
	if err == nil {
		t.Fatal("expected error")
	}
	var ce *Error
	if !errors.As(err, &ce) {
		t.Fatalf("expected *Error, got %T", err)
	}
	if ce.Code == statusOK {
		t.Fatal("error carried CU_OK status")
	}
	if ce.Error() == "" {
		t.Fatal("error message is empty")
	}
}

func TestDecompressWithCapacity(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			data := payloads()["text_18k"]
			c, err := Compress(a, data, DefaultLevel)
			if err != nil {
				t.Fatalf("Compress: %v", err)
			}
			for _, capacity := range []int{len(data), len(data) + 4096} {
				got, err := DecompressWithCapacity(a, c, capacity)
				if err != nil {
					t.Fatalf("capacity %d: %v", capacity, err)
				}
				if !bytes.Equal(got, data) {
					t.Fatalf("capacity %d: round-trip mismatch", capacity)
				}
			}
			if _, err := DecompressWithCapacity(a, c, len(data)/2); err == nil {
				t.Fatal("expected error for an undersized capacity")
			}
		})
	}
}

func TestLz4Raw(t *testing.T) {
	if !Available(Lz4Raw) {
		t.Skip("lz4_raw not available")
	}
	for name, data := range payloads() {
		t.Run(name, func(t *testing.T) {
			block, err := Compress(Lz4Raw, data, DefaultLevel)
			if err != nil {
				t.Fatalf("Compress: %v", err)
			}
			got, err := DecompressWithCapacity(Lz4Raw, block, len(data)+100)
			if err != nil {
				t.Fatalf("DecompressWithCapacity: %v", err)
			}
			if !bytes.Equal(got, data) {
				t.Fatal("round-trip mismatch")
			}
		})
	}
	block, _ := Compress(Lz4Raw, []byte("hello hello hello"), DefaultLevel)
	got, err := Decompress(Lz4Raw, block)
	if err != nil || !bytes.Equal(got, []byte("hello hello hello")) {
		t.Fatalf("incremental fallback: got %q, error %v", got, err)
	}
	var ce *Error
	if _, err := DecompressWithCapacity(Lz4Raw, block, 4); !errors.As(err, &ce) || ce.Code != ErrBufTooSmall {
		t.Fatalf("undersized capacity: want ErrBufTooSmall, got %v", err)
	}
}

func TestLzo(t *testing.T) {
	if !Available(Lzo) {
		t.Skip("lzo not available")
	}
	// Independent LZO1X block: five literals ("hello") then the end marker.
	block := []byte{22, 'h', 'e', 'l', 'l', 'o', 17, 0, 0}
	for _, decode := range []func() ([]byte, error){
		func() ([]byte, error) { return Decompress(Lzo, block) },
		func() ([]byte, error) { return DecompressWithCapacity(Lzo, block, 64) },
	} {
		got, err := decode()
		if err != nil {
			t.Fatalf("decode: %v", err)
		}
		if string(got) != "hello" {
			t.Fatalf("decode: got %q", got)
		}
	}
	var ce *Error
	if _, err := Compress(Lzo, []byte("hello"), DefaultLevel); !errors.As(err, &ce) || ce.Code != ErrUnsupported {
		t.Fatalf("Compress: want ErrUnsupported, got %v", err)
	}
}
