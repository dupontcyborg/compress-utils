package main

import (
	"bytes"
	"compress/bzip2"
	"compress/gzip"
	"compress/zlib"
	"fmt"
	"io"
	"sync"

	"github.com/andybalholm/brotli"
	dsbzip2 "github.com/dsnet/compress/bzip2"
	kgzip "github.com/klauspost/compress/gzip"
	ksnappy "github.com/klauspost/compress/snappy"
	kzlib "github.com/klauspost/compress/zlib"
	kzstd "github.com/klauspost/compress/zstd"
	"github.com/pierrec/lz4/v4"
	"github.com/ulikunitz/xz"

	cu "github.com/dupontcyborg/compress-utils/bindings/go"
)

// Codec is one implementation of one format. Decompress is never told the size.
// DStream decodes through the library's streaming API into w, in reads of at most
// `chunk` bytes. A nil Compress marks a decode-only library: the driver produces its
// input with compress-utils, untimed.
type Codec struct {
	Compress   func(data []byte, level int) ([]byte, error)
	Decompress func(comp []byte) ([]byte, error)
	CStream    func(data []byte, level int) ([]byte, error)
	DStream    func(comp []byte, w io.Writer) error
	Levels     *[2]int // native range; nil = levels.json's
	Levelless  bool
}

type impl struct {
	version string
	codecs  map[string]Codec
}

var implNames = []string{"compress-utils", "stdlib", "klauspost", "brotli", "lz4", "xz", "bzip2"}

func loadImpl(name string) (*impl, bool) {
	switch name {
	case "compress-utils":
		return implCompressUtils(), true
	case "stdlib":
		return implStdlib(), true
	case "klauspost":
		return implKlauspost(), true
	case "brotli":
		return implBrotli(), true
	case "lz4":
		return implLz4(), true
	case "xz":
		return implXz(), true
	case "bzip2":
		return implBzip2(), true
	}
	return nil, false
}

// ---- helpers ---------------------------------------------------------------------

// chunked feeds data to w in `chunk`-byte writes.
func chunked(w io.Writer, data []byte) error {
	for off := 0; off < len(data); off += chunk {
		end := min(off+chunk, len(data))
		if _, err := w.Write(data[off:end]); err != nil {
			return err
		}
	}
	return nil
}

// writeAll compresses through a writer: new(&buf) returns the writer, fed whole or
// in chunks, then closed.
func writeAll(mk func(io.Writer) (io.WriteCloser, error), data []byte, whole bool) ([]byte, error) {
	var buf bytes.Buffer
	buf.Grow(len(data)/2 + 64)
	w, err := mk(&buf)
	if err != nil {
		return nil, err
	}
	if whole {
		_, err = w.Write(data)
	} else {
		err = chunked(w, data)
	}
	if err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// countWriter counts bytes and discards them.
type countWriter struct{ n int }

func (c *countWriter) Write(p []byte) (int, error) { c.n += len(p); return len(p), nil }

// drainBuf is the reused read window of drain.
var drainBuf []byte

// drain reads r to the end into w through one reused window.
func drain(w io.Writer, r io.Reader) error {
	if drainBuf == nil {
		drainBuf = make([]byte, chunk)
	}
	_, err := io.CopyBuffer(w, r, drainBuf)
	return err
}

// streamCodec builds a Codec from a writer constructor and a reader constructor.
// Both modes go through the stream API; the one-shot one writes the input in a
// single call.
func streamCodec(mkW func(io.Writer, int) (io.WriteCloser, error),
	mkR func(io.Reader) (io.Reader, error)) Codec {
	enc := func(whole bool) func([]byte, int) ([]byte, error) {
		return func(d []byte, lvl int) ([]byte, error) {
			return writeAll(func(w io.Writer) (io.WriteCloser, error) { return mkW(w, lvl) }, d, whole)
		}
	}
	dstream := func(c []byte, w io.Writer) error {
		r, err := mkR(bytes.NewReader(c))
		if err != nil {
			return err
		}
		return drain(w, r)
	}
	return Codec{
		Compress: enc(true),
		Decompress: func(c []byte) ([]byte, error) {
			r, err := mkR(bytes.NewReader(c))
			if err != nil {
				return nil, err
			}
			return io.ReadAll(r)
		},
		CStream: enc(false),
		DStream: dstream,
	}
}

// ---- compress-utils --------------------------------------------------------------

func implCompressUtils() *impl {
	codec := func(algo cu.Algorithm) Codec {
		return Codec{
			Compress:   func(d []byte, lvl int) ([]byte, error) { return cu.Compress(algo, d, lvl) },
			Decompress: func(c []byte) ([]byte, error) { return cu.Decompress(algo, c) },
			CStream: func(d []byte, lvl int) ([]byte, error) {
				return writeAll(func(w io.Writer) (io.WriteCloser, error) { return cu.NewWriter(w, algo, lvl) }, d, false)
			},
			DStream: func(c []byte, w io.Writer) error {
				r, err := cu.NewReader(bytes.NewReader(c), algo)
				if err != nil {
					return err
				}
				defer r.Close()
				return drain(w, r)
			},
		}
	}
	m := map[string]Codec{}
	for name, a := range algos {
		m[name] = codec(a)
	}
	return &impl{version: cu.Version(), codecs: m}
}

// ---- standard library ------------------------------------------------------------

func implStdlib() *impl {
	zl := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) { return zlib.NewWriterLevel(w, lvl) },
		func(r io.Reader) (io.Reader, error) { return zlib.NewReader(r) })
	gz := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) { return gzip.NewWriterLevel(w, lvl) },
		func(r io.Reader) (io.Reader, error) { return gzip.NewReader(r) })
	// compress/bzip2 only decompresses.
	bz := Codec{Decompress: func(c []byte) ([]byte, error) {
		return io.ReadAll(bzip2.NewReader(bytes.NewReader(c)))
	}, DStream: func(c []byte, w io.Writer) error {
		return drain(w, bzip2.NewReader(bytes.NewReader(c)))
	}}
	return &impl{version: goVersion(), codecs: map[string]Codec{"zlib": zl, "gzip": gz, "bz2": bz}}
}

// ---- klauspost/compress ----------------------------------------------------------

func implKlauspost() *impl {
	zl := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) { return kzlib.NewWriterLevel(w, lvl) },
		func(r io.Reader) (io.Reader, error) { return kzlib.NewReader(r) })
	gz := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) { return kgzip.NewWriterLevel(w, lvl) },
		func(r io.Reader) (io.Reader, error) { return kgzip.NewReader(r) })

	// One single-threaded encoder per level and one decoder, reused: the library's
	// documented usage. EncodeAll frames carry the content size.
	var mu sync.Mutex
	encs := map[int]*kzstd.Encoder{}
	enc := func(lvl int) *kzstd.Encoder {
		mu.Lock()
		defer mu.Unlock()
		if e, ok := encs[lvl]; ok {
			return e
		}
		e, err := kzstd.NewWriter(nil, kzstd.WithEncoderLevel(kzstd.EncoderLevelFromZstd(lvl)),
			kzstd.WithEncoderConcurrency(1))
		if err != nil {
			panic(err)
		}
		encs[lvl] = e
		return e
	}
	dec, err := kzstd.NewReader(nil, kzstd.WithDecoderConcurrency(1))
	if err != nil {
		panic(err)
	}
	zs := Codec{
		Compress:   func(d []byte, lvl int) ([]byte, error) { return enc(lvl).EncodeAll(d, nil), nil },
		Decompress: func(c []byte) ([]byte, error) { return dec.DecodeAll(c, nil) },
		CStream: func(d []byte, lvl int) ([]byte, error) {
			e := enc(lvl)
			var buf bytes.Buffer
			e.Reset(&buf)
			if err := chunked(e, d); err != nil {
				return nil, err
			}
			if err := e.Close(); err != nil {
				return nil, err
			}
			return buf.Bytes(), nil
		},
		DStream: func(c []byte, w io.Writer) error {
			d, err := kzstd.NewReader(bytes.NewReader(c), kzstd.WithDecoderConcurrency(1))
			if err != nil {
				return err
			}
			defer d.Close()
			return drain(w, d)
		},
		Levels: &[2]int{1, 22},
	}

	// Raw snappy blocks (compress-utils' format); the library's stream form is the
	// framing format, a different one, so no streaming.
	sn := Codec{
		Compress:   func(d []byte, _ int) ([]byte, error) { return ksnappy.Encode(nil, d), nil },
		Decompress: func(c []byte) ([]byte, error) { return ksnappy.Decode(nil, c) },
		Levelless:  true,
	}
	return &impl{version: "klauspost/compress " + modVersion("github.com/klauspost/compress"),
		codecs: map[string]Codec{"zlib": zl, "gzip": gz, "zstd": zs, "snappy": sn}}
}

// ---- andybalholm/brotli ----------------------------------------------------------

func implBrotli() *impl {
	c := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) { return brotli.NewWriterLevel(w, lvl), nil },
		func(r io.Reader) (io.Reader, error) { return brotli.NewReader(r), nil })
	c.Levels = &[2]int{0, 11}
	return &impl{version: "andybalholm/brotli " + modVersion("github.com/andybalholm/brotli"),
		codecs: map[string]Codec{"brotli": c}}
}

// ---- pierrec/lz4 -----------------------------------------------------------------

func implLz4() *impl {
	levels := []lz4.CompressionLevel{lz4.Fast, lz4.Level1, lz4.Level2, lz4.Level3, lz4.Level4,
		lz4.Level5, lz4.Level6, lz4.Level7, lz4.Level8, lz4.Level9}
	enc := func(withSize bool) func([]byte, int) ([]byte, error) {
		return func(d []byte, lvl int) ([]byte, error) {
			if lvl < 0 || lvl >= len(levels) {
				return nil, fmt.Errorf("lz4 level %d out of range", lvl)
			}
			// LZ4 frames like compress-utils': content checksum and, in one-shot,
			// the content size.
			return writeAll(func(w io.Writer) (io.WriteCloser, error) {
				zw := lz4.NewWriter(w)
				opts := []lz4.Option{lz4.CompressionLevelOption(levels[lvl]), lz4.ChecksumOption(true),
					lz4.ConcurrencyOption(1)}
				if withSize {
					opts = append(opts, lz4.SizeOption(uint64(len(d))))
				}
				return zw, zw.Apply(opts...)
			}, d, withSize)
		}
	}
	reader := func(c []byte) (io.Reader, error) {
		zr := lz4.NewReader(bytes.NewReader(c))
		return zr, zr.Apply(lz4.ConcurrencyOption(1))
	}
	return &impl{version: "pierrec/lz4 " + modVersion("github.com/pierrec/lz4/v4"), codecs: map[string]Codec{
		"lz4": {Compress: enc(true), CStream: enc(false), Levels: &[2]int{0, 9},
			Decompress: func(c []byte) ([]byte, error) {
				r, err := reader(c)
				if err != nil {
					return nil, err
				}
				return io.ReadAll(r)
			},
			DStream: func(c []byte, w io.Writer) error {
				r, err := reader(c)
				if err != nil {
					return err
				}
				return drain(w, r)
			}}}}
}

// ---- ulikunitz/xz ----------------------------------------------------------------

func implXz() *impl {
	// Pure Go; the encoder takes no preset, only a dictionary size, so no levels.
	c := streamCodec(
		func(w io.Writer, _ int) (io.WriteCloser, error) {
			return xz.WriterConfig{CheckSum: xz.CRC64}.NewWriter(w)
		},
		func(r io.Reader) (io.Reader, error) { return xz.NewReader(r) })
	c.Levelless = true
	return &impl{version: "ulikunitz/xz " + modVersion("github.com/ulikunitz/xz"),
		codecs: map[string]Codec{"xz": c}}
}

// ---- dsnet/compress bzip2 --------------------------------------------------------

func implBzip2() *impl {
	c := streamCodec(
		func(w io.Writer, lvl int) (io.WriteCloser, error) {
			return dsbzip2.NewWriter(w, &dsbzip2.WriterConfig{Level: lvl})
		},
		func(r io.Reader) (io.Reader, error) { return dsbzip2.NewReader(r, nil) })
	c.Levels = &[2]int{1, 9}
	return &impl{version: "dsnet/compress " + modVersion("github.com/dsnet/compress"),
		codecs: map[string]Codec{"bz2": c}}
}
