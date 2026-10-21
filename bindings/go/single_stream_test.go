package compressutils

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"testing"
)

// trailer stands in for whatever follows a message: the next message's
// header, then data.
var trailer = append([]byte("ZPKL\x01\x01\x03\x00"), make([]byte, 24)...)

// chunkReader returns its data in reads of at most limit bytes, like a
// fragmenting transport.
type chunkReader struct {
	data  []byte
	limit int
}

func (c *chunkReader) Read(p []byte) (int, error) {
	if len(c.data) == 0 {
		return 0, io.EOF
	}
	if len(p) > c.limit {
		p = p[:c.limit]
	}
	n := copy(p, c.data)
	c.data = c.data[n:]
	return n, nil
}

// splitReader returns its pieces one Read at a time, never merging them.
type splitReader struct{ pieces [][]byte }

func (s *splitReader) Read(p []byte) (int, error) {
	for len(s.pieces) > 0 && len(s.pieces[0]) == 0 {
		s.pieces = s.pieces[1:]
	}
	if len(s.pieces) == 0 {
		return 0, io.EOF
	}
	n := copy(p, s.pieces[0])
	s.pieces[0] = s.pieces[0][n:]
	return n, nil
}

func singleReader(t *testing.T, src io.Reader, a Algorithm) *Reader {
	t.Helper()
	r, err := NewReader(src, a)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	r.Multistream(false)
	return r
}

func mustCompress(t *testing.T, a Algorithm, p []byte) []byte {
	t.Helper()
	c, err := Compress(a, p, DefaultLevel)
	if err != nil {
		t.Fatalf("Compress: %v", err)
	}
	return c
}

// rest is what a caller continues reading after r's stream ended.
func rest(t *testing.T, r *Reader, src io.Reader) []byte {
	t.Helper()
	b, err := io.ReadAll(io.MultiReader(bytes.NewReader(r.Unused()), src))
	if err != nil {
		t.Fatalf("reading past the end: %v", err)
	}
	return b
}

// TestSingleStream: Multistream(false) stops at the end of the first stream,
// and Unused plus the rest of the source is exactly what followed it, however
// the source fragments its reads.
func TestSingleStream(t *testing.T) {
	random := make([]byte, 300000) // incompressible: frames bigger than the 64 KiB read buffer
	rand.New(rand.NewSource(3)).Read(random)
	plain := map[string][]byte{"empty": {}, "text": bytes.Repeat([]byte("single stream "), 500), "random_300k": random}
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		for name, p := range plain {
			frame := mustCompress(t, a, p)
			input := append(append([]byte(nil), frame...), trailer...)
			sources := map[string]func() io.Reader{
				"one_source": func() io.Reader { return bytes.NewReader(input) },
				"split_at_end": func() io.Reader {
					return &splitReader{pieces: [][]byte{append([]byte(nil), frame...), append([]byte(nil), trailer...)}}
				},
				// The frame ends partway through one of the Reader's reads.
				"reads_of_7":    func() io.Reader { return &chunkReader{data: input, limit: 7} },
				"reads_of_4093": func() io.Reader { return &chunkReader{data: input, limit: 4093} },
			}
			for sname, mk := range sources {
				t.Run(fmt.Sprintf("%s/%s/%s", a.Name(), name, sname), func(t *testing.T) {
					src := mk()
					r := singleReader(t, src, a)
					defer r.Close()
					got, err := io.ReadAll(r)
					if err != nil {
						t.Fatalf("ReadAll: %v", err)
					}
					if !bytes.Equal(got, p) {
						t.Fatalf("decoded %d bytes, want %d", len(got), len(p))
					}
					if after := rest(t, r, src); !bytes.Equal(after, trailer) {
						t.Fatalf("after the stream: %q, want %q", after, trailer)
					}
					if sname == "split_at_end" && len(r.Unused()) != 0 {
						t.Fatalf("read past a source boundary at the end: unused %d bytes", len(r.Unused()))
					}
				})
			}
		}
	}
}

// TestSingleStreamBackToBack: two frames then a trailer; each Reader stops at
// its frame, and the second picks up from the first's Unused, including when
// the first frame is larger than the Reader's read buffer.
func TestSingleStreamBackToBack(t *testing.T) {
	big := make([]byte, 1<<20)
	rand.New(rand.NewSource(5)).Read(big)
	second := bytes.Repeat([]byte("second message "), 2000)
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			f1, f2 := mustCompress(t, a, big), mustCompress(t, a, second)
			input := append(append(append([]byte(nil), f1...), f2...), trailer...)
			for _, limit := range []int{len(input), 65536 + 13, 4093} {
				var src io.Reader = &chunkReader{data: input, limit: limit}
				for i, want := range [][]byte{big, second} {
					r := singleReader(t, src, a)
					got, err := io.ReadAll(r)
					if err != nil {
						t.Fatalf("limit %d, frame %d: ReadAll: %v", limit, i, err)
					}
					if !bytes.Equal(got, want) {
						t.Fatalf("limit %d, frame %d: decoded %d bytes, want %d", limit, i, len(got), len(want))
					}
					src = io.MultiReader(bytes.NewReader(r.Unused()), src)
					r.Close()
				}
				if after, _ := io.ReadAll(src); !bytes.Equal(after, trailer) {
					t.Fatalf("limit %d: after both frames: %d bytes, want the trailer", limit, len(after))
				}
			}
		})
	}
}

// TestSingleStreamErrors: truncation, the codecs without single mode, and
// Multistream after Read.
func TestSingleStreamErrors(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		frame := mustCompress(t, a, bytes.Repeat([]byte("truncated "), 100))
		r := singleReader(t, bytes.NewReader(frame[:len(frame)-1]), a)
		if _, err := io.ReadAll(r); err == nil {
			t.Fatalf("%s: truncated frame decoded without error", a.Name())
		}
		if len(r.Unused()) != 0 {
			t.Fatalf("%s: unused bytes without an end", a.Name())
		}
		r.Close()
	}

	for _, a := range []Algorithm{Lz4Raw, Lzo} {
		if !Available(a) {
			continue
		}
		r := singleReader(t, bytes.NewReader([]byte{0}), a)
		var ce *Error
		if _, err := r.Read(make([]byte, 8)); !errors.As(err, &ce) || ce.Code != ErrUnsupported {
			t.Fatalf("%s: got %v, want ErrUnsupported", a.Name(), err)
		}
		r.Close()
	}

	r, err := NewReader(bytes.NewReader(mustCompress(t, Zstd, []byte("x"))), Zstd)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	defer r.Close()
	r.Read(make([]byte, 1))
	defer func() {
		if recover() == nil {
			t.Fatal("Multistream after Read didn't panic")
		}
	}()
	r.Multistream(false)
}

// TestMultistreamDefault: without Multistream(false), concatenated frames
// still decode as one stream and Unused stays empty.
func TestMultistreamDefault(t *testing.T) {
	for _, a := range []Algorithm{Zstd, Lz4, Gzip, Xz} {
		if !Available(a) {
			continue
		}
		p1, p2 := []byte("first frame "), []byte("second frame")
		input := append(mustCompress(t, a, p1), mustCompress(t, a, p2)...)
		for _, explicit := range []bool{false, true} {
			r, err := NewReader(bytes.NewReader(input), a)
			if err != nil {
				t.Fatalf("NewReader: %v", err)
			}
			if explicit {
				r.Multistream(false)
				r.Multistream(true)
			}
			got, err := io.ReadAll(r)
			if err != nil || !bytes.Equal(got, append(append([]byte(nil), p1...), p2...)) {
				t.Fatalf("%s: got %q, %v", a.Name(), got, err)
			}
			if r.Unused() != nil {
				t.Fatalf("%s: unused in multistream mode", a.Name())
			}
			r.Close()
		}
	}
}
