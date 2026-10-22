package compressutils

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"testing"
	"testing/iotest"
)

// defaultMaxDecompressedSize mirrors CU_DEFAULT_MAX_DECOMPRESSED_SIZE in
// src/compress_utils.c (1 GiB), the value SetMaxDecompressedSize documents.
const defaultMaxDecompressedSize = 1 << 30

// setMaxDecompressedSize changes the process-global cap for the rest of the
// test and restores the default afterwards. Tests using it must not run in
// parallel.
func setMaxDecompressedSize(t *testing.T, n uint64) {
	t.Helper()
	t.Cleanup(func() { SetMaxDecompressedSize(defaultMaxDecompressedSize) })
	SetMaxDecompressedSize(n)
}

// wantStatus fails unless err is an *Error carrying code.
func wantStatus(t *testing.T, what string, err error, code Status) {
	t.Helper()
	var ce *Error
	if !errors.As(err, &ce) {
		t.Fatalf("%s: want *Error with code %d, got %T (%v)", what, int(code), err, err)
	}
	if ce.Code != code {
		t.Fatalf("%s: want code %d, got %d (%v)", what, int(code), int(ce.Code), ce)
	}
	if ce.Error() == "" {
		t.Fatalf("%s: empty error message", what)
	}
}

// failWriter accepts the first ok writes and then fails every later one.
type failWriter struct {
	ok  int
	err error
	buf bytes.Buffer
}

func (f *failWriter) Write(p []byte) (int, error) {
	if f.ok == 0 {
		return 0, f.err
	}
	f.ok--
	return f.buf.Write(p)
}

const unknownAlgo = Algorithm(99)

func TestAlgorithmString(t *testing.T) {
	names := map[Algorithm]string{
		Zstd: "zstd", Brotli: "brotli", Zlib: "zlib", Bz2: "bz2", Lz4: "lz4",
		Xz: "xz", Snappy: "snappy", Gzip: "gzip", Deflate: "deflate",
	}
	for a, want := range names {
		if !Available(a) {
			continue
		}
		if got := a.Name(); got != want {
			t.Errorf("Algorithm(%d).Name() = %q, want %q", int(a), got, want)
		}
		// String makes Algorithm a fmt.Stringer that prints the name.
		if got := fmt.Sprint(a); got != want {
			t.Errorf("fmt.Sprint(Algorithm(%d)) = %q, want %q", int(a), got, want)
		}
	}

	for _, a := range []Algorithm{unknownAlgo, Algorithm(-1)} {
		if Available(a) {
			t.Fatalf("Algorithm(%d) unexpectedly available", int(a))
		}
		if got := a.Name(); got != "" {
			t.Errorf("Algorithm(%d).Name() = %q, want \"\"", int(a), got)
		}
		want := fmt.Sprintf("Algorithm(%d)", int(a))
		if got := a.String(); got != want {
			t.Errorf("Algorithm(%d).String() = %q, want %q", int(a), got, want)
		}
	}
}

// TestUnknownAlgorithm: every entry point rejects an algorithm that isn't in
// the build with ErrUnsupported.
func TestUnknownAlgorithm(t *testing.T) {
	data := []byte("hello")
	_, err := Compress(unknownAlgo, data, DefaultLevel)
	wantStatus(t, "Compress", err, ErrUnsupported)
	_, err = Decompress(unknownAlgo, data)
	wantStatus(t, "Decompress", err, ErrUnsupported)
	_, err = DecompressWithCapacity(unknownAlgo, data, 64)
	wantStatus(t, "DecompressWithCapacity", err, ErrUnsupported)

	w, err := NewWriter(io.Discard, unknownAlgo, DefaultLevel)
	if w != nil {
		t.Fatal("NewWriter returned a Writer alongside an error")
	}
	wantStatus(t, "NewWriter", err, ErrUnsupported)

	r, err := NewReader(bytes.NewReader(data), unknownAlgo)
	if r != nil {
		t.Fatal("NewReader returned a Reader alongside an error")
	}
	wantStatus(t, "NewReader", err, ErrUnsupported)
}

// TestInvalidLevel: levels outside 1..10 are rejected by both compressors.
func TestInvalidLevel(t *testing.T) {
	if !Available(Zstd) {
		t.Skip("zstd not available")
	}
	for _, level := range []int{0, 11, -1} {
		_, err := Compress(Zstd, []byte("hello"), level)
		wantStatus(t, fmt.Sprintf("Compress level %d", level), err, ErrInvalidLevel)
		w, err := NewWriter(io.Discard, Zstd, level)
		if w != nil {
			t.Fatalf("NewWriter level %d returned a Writer alongside an error", level)
		}
		wantStatus(t, fmt.Sprintf("NewWriter level %d", level), err, ErrInvalidLevel)
	}
}

func TestDecompressWithCapacityNegative(t *testing.T) {
	c := mustCompress(t, Zstd, []byte("hello"))
	got, err := DecompressWithCapacity(Zstd, c, -1)
	if got != nil {
		t.Fatalf("got %q alongside an error", got)
	}
	wantStatus(t, "negative capacity", err, ErrInvalidArg)
}

// TestDecompressConcatenatedZstd: concatenated zstd frames decode as one
// output through the one-shot API, as they do through the Reader.
func TestDecompressConcatenatedZstd(t *testing.T) {
	if !Available(Zstd) {
		t.Skip("zstd not available")
	}
	p1 := bytes.Repeat([]byte("first frame "), 300)
	p2 := bytes.Repeat([]byte("second frame "), 500)
	input := append(mustCompress(t, Zstd, p1), mustCompress(t, Zstd, p2)...)
	want := append(append([]byte(nil), p1...), p2...)

	got, err := Decompress(Zstd, input)
	if err != nil {
		t.Fatalf("Decompress: %v", err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("got %d bytes, want %d", len(got), len(want))
	}
	got, err = DecompressWithCapacity(Zstd, input, len(want))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("DecompressWithCapacity: %d bytes, err %v", len(got), err)
	}
}

// TestSetMaxDecompressedSize: the cap rejects one-shot output larger than it
// with ErrSizeLimit, 0 disables it, and the default (1 GiB) admits ordinary
// payloads again.
func TestSetMaxDecompressedSize(t *testing.T) {
	data := bytes.Repeat([]byte("The quick brown fox jumps over the lazy dog. "), 400)
	// zstd, LZ4 and Snappy record the decompressed size, so Decompress takes
	// the one-shot path; the others decode through a stream, which the cap
	// bounds too.
	for _, a := range []Algorithm{Zstd, Lz4, Snappy, Zlib, Gzip, Deflate, Brotli, Bz2, Xz} {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			c := mustCompress(t, a, data)

			setMaxDecompressedSize(t, uint64(len(data)-1))
			got, err := Decompress(a, c)
			if got != nil {
				t.Fatalf("Decompress over the cap returned %d bytes", len(got))
			}
			wantStatus(t, "Decompress over the cap", err, ErrSizeLimit)
			_, err = DecompressWithCapacity(a, c, len(data))
			wantStatus(t, "DecompressWithCapacity over the cap", err, ErrSizeLimit)

			// Exactly at the cap is allowed.
			SetMaxDecompressedSize(uint64(len(data)))
			if got, err := Decompress(a, c); err != nil || !bytes.Equal(got, data) {
				t.Fatalf("Decompress at the cap: %d bytes, err %v", len(got), err)
			}

			SetMaxDecompressedSize(0) // disabled
			if got, err := Decompress(a, c); err != nil || !bytes.Equal(got, data) {
				t.Fatalf("Decompress with the cap disabled: %d bytes, err %v", len(got), err)
			}

			SetMaxDecompressedSize(defaultMaxDecompressedSize)
			if got, err := Decompress(a, c); err != nil || !bytes.Equal(got, data) {
				t.Fatalf("Decompress with the default cap: %d bytes, err %v", len(got), err)
			}
		})
	}

	// Raw LZ4 records no size: Decompress falls back to the incremental
	// decoder, which captures the cap when it is created.
	if Available(Lz4Raw) {
		block := mustCompress(t, Lz4Raw, data)
		setMaxDecompressedSize(t, uint64(len(data)-1))
		_, err := Decompress(Lz4Raw, block)
		wantStatus(t, "lz4_raw Decompress over the cap", err, ErrSizeLimit)
		_, err = DecompressWithCapacity(Lz4Raw, block, len(data))
		wantStatus(t, "lz4_raw DecompressWithCapacity over the cap", err, ErrSizeLimit)
		SetMaxDecompressedSize(0)
		if got, err := Decompress(Lz4Raw, block); err != nil || !bytes.Equal(got, data) {
			t.Fatalf("lz4_raw Decompress with the cap disabled: %d bytes, err %v", len(got), err)
		}
	}
}

// TestSetMaxDecompressedSizeStreaming: the cap doesn't limit the streaming
// Reader for codecs that don't capture it (all but Snappy, raw LZ4 and LZO,
// per cu_set_max_decompressed_size).
func TestSetMaxDecompressedSizeStreaming(t *testing.T) {
	data := bytes.Repeat([]byte("stream past the one-shot cap "), 1000)
	setMaxDecompressedSize(t, 16)
	for _, a := range []Algorithm{Zstd, Zlib, Gzip, Brotli, Bz2, Xz, Lz4, Deflate} {
		if !Available(a) {
			continue
		}
		c := mustCompress(t, a, data)
		r, err := NewReader(bytes.NewReader(c), a)
		if err != nil {
			t.Fatalf("%s: NewReader: %v", a.Name(), err)
		}
		got, err := io.ReadAll(r)
		r.Close()
		if err != nil || !bytes.Equal(got, data) {
			t.Fatalf("%s: Reader under a 16-byte cap: %d bytes, err %v", a.Name(), len(got), err)
		}
	}
}

// TestReaderSetMaxOutput: a Reader stops with ErrSizeLimit after exactly its
// limit, also across Multistream, and decodes everything at the limit.
func TestReaderSetMaxOutput(t *testing.T) {
	data := bytes.Repeat([]byte("stream past the reader limit "), 1000)
	for _, a := range []Algorithm{Zstd, Zlib, Gzip, Brotli, Bz2, Xz, Lz4, Snappy, Deflate} {
		if !Available(a) {
			continue
		}
		c := mustCompress(t, a, data)
		for _, single := range []bool{false, true} {
			r, err := NewReader(bytes.NewReader(c), a)
			if err != nil {
				t.Fatalf("%s: NewReader: %v", a.Name(), err)
			}
			r.SetMaxOutput(uint64(len(data) - 1))
			r.Multistream(!single) // recreates the C stream; the limit must survive
			got, err := io.ReadAll(r)
			r.Close()
			wantStatus(t, a.Name()+" Reader over its limit", err, ErrSizeLimit)
			if !bytes.Equal(got, data[:len(data)-1]) {
				t.Fatalf("%s: Reader over its limit returned %d bytes, want %d", a.Name(), len(got), len(data)-1)
			}

			r, _ = NewReader(bytes.NewReader(c), a)
			r.Multistream(!single)
			r.SetMaxOutput(uint64(len(data)))
			got, err = io.ReadAll(r)
			r.Close()
			if err != nil || !bytes.Equal(got, data) {
				t.Fatalf("%s: Reader at its limit: %d bytes, err %v", a.Name(), len(got), err)
			}
		}
	}
}

// TestWriterLifecycle: empty writes are no-ops, Close is idempotent, and a
// closed Writer rejects further writes and flushes.
func TestWriterLifecycle(t *testing.T) {
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		t.Run(a.Name(), func(t *testing.T) {
			var sink bytes.Buffer
			w, err := NewWriter(&sink, a, DefaultLevel)
			if err != nil {
				t.Fatalf("NewWriter: %v", err)
			}
			if n, err := w.Write(nil); n != 0 || err != nil {
				t.Fatalf("Write(nil) = %d, %v; want 0, nil", n, err)
			}
			if n, err := w.Write([]byte{}); n != 0 || err != nil {
				t.Fatalf("Write(empty) = %d, %v; want 0, nil", n, err)
			}
			msg := []byte("lifecycle")
			if n, err := w.Write(msg); n != len(msg) || err != nil {
				t.Fatalf("Write = %d, %v; want %d, nil", n, err, len(msg))
			}
			if err := w.Close(); err != nil {
				t.Fatalf("Close: %v", err)
			}
			frame := append([]byte(nil), sink.Bytes()...)
			if err := w.Close(); err != nil {
				t.Fatalf("second Close: %v", err)
			}
			if n, err := w.Write(msg); n != 0 || err != errClosed {
				t.Fatalf("Write after Close = %d, %v; want 0, errClosed", n, err)
			}
			if err := w.Flush(); err != errClosed {
				t.Fatalf("Flush after Close: %v, want errClosed", err)
			}
			if !bytes.Equal(sink.Bytes(), frame) {
				t.Fatal("sink changed after Close")
			}
			got, err := Decompress(a, frame)
			if err != nil || !bytes.Equal(got, msg) {
				t.Fatalf("Decompress: %q, %v", got, err)
			}
		})
	}
}

// TestWriterSinkErrors: an error from the sink is returned unchanged by
// Write, Flush and Close, and Close still releases the stream.
func TestWriterSinkErrors(t *testing.T) {
	if !Available(Zstd) {
		t.Skip("zstd not available")
	}
	sinkErr := errors.New("sink failed")
	// Incompressible, so zstd emits output during Write.
	big := make([]byte, 1<<20)
	for i := range big {
		big[i] = byte(i*2654435761>>13) ^ byte(i>>5)
	}

	t.Run("write", func(t *testing.T) {
		w, err := NewWriter(&failWriter{err: sinkErr}, Zstd, DefaultLevel)
		if err != nil {
			t.Fatalf("NewWriter: %v", err)
		}
		defer w.Close()
		if n, err := w.Write(big); n != 0 || err != sinkErr {
			t.Fatalf("Write = %d, %v; want 0, sink error", n, err)
		}
	})

	t.Run("flush", func(t *testing.T) {
		w, err := NewWriter(&failWriter{err: sinkErr}, Zstd, DefaultLevel)
		if err != nil {
			t.Fatalf("NewWriter: %v", err)
		}
		defer w.Close()
		if _, err := w.Write([]byte("small")); err != nil {
			t.Fatalf("Write: %v", err)
		}
		if err := w.Flush(); err != sinkErr {
			t.Fatalf("Flush: %v, want sink error", err)
		}
	})

	t.Run("close", func(t *testing.T) {
		w, err := NewWriter(&failWriter{err: sinkErr}, Zstd, DefaultLevel)
		if err != nil {
			t.Fatalf("NewWriter: %v", err)
		}
		if _, err := w.Write([]byte("small")); err != nil {
			t.Fatalf("Write: %v", err)
		}
		if err := w.Close(); err != sinkErr {
			t.Fatalf("Close: %v, want sink error", err)
		}
		// The stream is released even though finishing failed.
		if err := w.Close(); err != nil {
			t.Fatalf("second Close: %v", err)
		}
		if _, err := w.Write([]byte("x")); err != errClosed {
			t.Fatalf("Write after failed Close: %v, want errClosed", err)
		}
	})
}

// TestReaderSourceError: an error from the source is returned by Read, and
// keeps being returned.
func TestReaderSourceError(t *testing.T) {
	srcErr := errors.New("source failed")
	c := mustCompress(t, Zstd, bytes.Repeat([]byte("abc"), 1000))
	src := io.MultiReader(bytes.NewReader(c[:len(c)/2]), iotest.ErrReader(srcErr))
	r, err := NewReader(src, Zstd)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	defer r.Close()
	got, err := io.ReadAll(r)
	if err != srcErr {
		t.Fatalf("ReadAll: err %v, want source error", err)
	}
	if !bytes.HasPrefix(bytes.Repeat([]byte("abc"), 1000), got) {
		t.Fatal("output before the error isn't a prefix of the payload")
	}
	if n, err := r.Read(make([]byte, 8)); n != 0 || err != srcErr {
		t.Fatalf("Read after error = %d, %v; want 0, source error", n, err)
	}
}

// TestReaderDataWithEOF: a source that returns its last bytes together with
// io.EOF (allowed by io.Reader) still decodes completely.
func TestReaderDataWithEOF(t *testing.T) {
	data := bytes.Repeat([]byte("data and EOF together "), 500)
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		c := mustCompress(t, a, data)
		for _, single := range []bool{false, true} {
			r, err := NewReader(iotest.DataErrReader(bytes.NewReader(c)), a)
			if err != nil {
				t.Fatalf("%s: NewReader: %v", a.Name(), err)
			}
			if single {
				r.Multistream(false)
			}
			got, err := io.ReadAll(r)
			r.Close()
			if err != nil || !bytes.Equal(got, data) {
				t.Fatalf("%s (single=%v): %d bytes, err %v", a.Name(), single, len(got), err)
			}
		}
	}
}

// TestReaderLargeOutput: output many times the 64 KiB scratch buffer, read
// through the default and single-stream modes, round-trips for every codec.
func TestReaderLargeOutput(t *testing.T) {
	data := make([]byte, 1<<20)
	for i := range data {
		data[i] = byte(i % 251)
	}
	for _, a := range allAlgos {
		if !Available(a) {
			continue
		}
		c := mustCompress(t, a, data)
		for _, single := range []bool{false, true} {
			r, err := NewReader(bytes.NewReader(c), a)
			if err != nil {
				t.Fatalf("%s: NewReader: %v", a.Name(), err)
			}
			if single {
				r.Multistream(false)
			}
			got, err := io.ReadAll(r)
			r.Close()
			if err != nil || !bytes.Equal(got, data) {
				t.Fatalf("%s (single=%v): %d bytes, err %v", a.Name(), single, len(got), err)
			}
		}
	}
}

// TestReaderClose: Close is idempotent, Multistream on a closed Reader is a
// no-op.
func TestReaderClose(t *testing.T) {
	c := mustCompress(t, Zstd, []byte("closed"))
	r, err := NewReader(bytes.NewReader(c), Zstd)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("second Close: %v", err)
	}
	r.Multistream(false) // must not panic or recreate the stream
	if r.stream != nil {
		t.Fatal("Multistream recreated the stream of a closed Reader")
	}
	if r.single {
		t.Fatal("Multistream changed the mode of a closed Reader")
	}
}

// TestMultistreamTrueIsDefault: Multistream(true) on a fresh Reader keeps the
// default mode, so concatenated frames still decode as one stream.
func TestMultistreamTrueIsDefault(t *testing.T) {
	p1, p2 := []byte("one "), []byte("two")
	input := append(mustCompress(t, Zstd, p1), mustCompress(t, Zstd, p2)...)
	r, err := NewReader(bytes.NewReader(input), Zstd)
	if err != nil {
		t.Fatalf("NewReader: %v", err)
	}
	defer r.Close()
	r.Multistream(true)
	got, err := io.ReadAll(r)
	if err != nil || string(got) != "one two" {
		t.Fatalf("got %q, %v", got, err)
	}
	if r.Unused() != nil {
		t.Fatal("Unused in multistream mode")
	}
}
