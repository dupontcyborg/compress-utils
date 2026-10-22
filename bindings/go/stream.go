package compressutils

/*
#include "compress_utils.h"
*/
import "C"

import (
	"bytes"
	"errors"
	"io"
	"runtime"
	"unsafe"
)

// streamScratch is the size of the fixed output buffer each stream drains
// through. The C ABI's "fill buffer, return BUF_TOO_SMALL, drain" protocol
// works with any size; 64 KiB keeps the cgo call count low without holding
// much memory.
const streamScratch = 64 * 1024

var errClosed = errors.New("compressutils: stream is closed")

// Writer is an io.WriteCloser that compresses everything written to it and
// forwards the compressed bytes to an underlying sink. Close MUST be called
// to flush the final frame; a Writer that is never closed produces a
// truncated, undecodable stream.
//
// A Writer is not safe for concurrent use.
type Writer struct {
	sink    io.Writer
	stream  *C.cu_compress_stream_t
	scratch []byte
	closed  bool
}

// NewWriter returns a Writer that compresses with the given algorithm at the
// given level (1 fastest .. 10 smallest) into sink.
func NewWriter(sink io.Writer, algo Algorithm, level int) (*Writer, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var s *C.cu_compress_stream_t
	st := C.cu_compress_stream_create(C.cu_algorithm_t(algo), C.int(level), &s)
	if err := statusErr(st); err != nil {
		return nil, err
	}
	w := &Writer{sink: sink, stream: s, scratch: make([]byte, streamScratch)}
	// Safety net: free the C stream if the caller forgets Close. This does
	// not flush (a finalizer must not touch the sink) — an unclosed frame is
	// already lost — it only reclaims the C allocation.
	runtime.SetFinalizer(w, (*Writer).free)
	return w, nil
}

// Write compresses p, forwarding output to the sink. It returns len(p) and
// nil on success; a short count only accompanies a non-nil error.
func (w *Writer) Write(p []byte) (int, error) {
	if w.closed {
		return 0, errClosed
	}
	if len(p) == 0 {
		return 0, nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	first := true
	for {
		outLen := C.size_t(len(w.scratch))
		var st C.cu_status_t
		if first {
			st = C.cu_compress_stream_write(w.stream,
				bytePtr(p), C.size_t(len(p)), bytePtr(w.scratch), &outLen)
			first = false
		} else {
			// Drain: the stream retained the unconsumed tail of p internally.
			st = C.cu_compress_stream_write(w.stream,
				nil, 0, bytePtr(w.scratch), &outLen)
		}
		if n := int(outLen); n > 0 {
			if _, err := w.sink.Write(w.scratch[:n]); err != nil {
				return 0, err
			}
		}
		if st == C.CU_OK {
			return len(p), nil
		}
		if Status(st) != ErrBufTooSmall {
			return 0, statusErr(st)
		}
	}
}

// Flush writes everything a decoder needs to recover all data written so far
// to the sink, keeping the frame open for more writes; use it to deliver a
// message on a long-lived stream. Like compress/flate's Writer.Flush, it does
// not flush the sink itself. Each flush costs a few bytes of output.
//
// Snappy and Bz2 can't flush mid-stream and return an *Error with code
// ErrUnsupported.
func (w *Writer) Flush() error {
	if w.closed {
		return errClosed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	for {
		outLen := C.size_t(len(w.scratch))
		st := C.cu_compress_stream_flush(w.stream, bytePtr(w.scratch), &outLen)
		if n := int(outLen); n > 0 {
			if _, err := w.sink.Write(w.scratch[:n]); err != nil {
				return err
			}
		}
		if st == C.CU_OK {
			return nil
		}
		if Status(st) != ErrBufTooSmall {
			return statusErr(st)
		}
	}
}

// Close flushes any buffered data, finalizes the frame, and releases the
// underlying C stream. It is safe to call more than once.
func (w *Writer) Close() error {
	if w.closed {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var retErr error
	for {
		outLen := C.size_t(len(w.scratch))
		st := C.cu_compress_stream_finish(w.stream, bytePtr(w.scratch), &outLen)
		if n := int(outLen); n > 0 {
			if _, err := w.sink.Write(w.scratch[:n]); err != nil {
				retErr = err
				break
			}
		}
		if st == C.CU_OK {
			break
		}
		if Status(st) != ErrBufTooSmall {
			retErr = statusErr(st)
			break
		}
	}
	w.free()
	return retErr
}

// free destroys the C stream. Idempotent. Runs under a locked OS thread
// when called from Close; the finalizer path calls it on its own goroutine,
// where a bare destroy (no last-error read) is safe.
func (w *Writer) free() {
	if w.stream != nil {
		C.cu_compress_stream_destroy(w.stream)
		w.stream = nil
	}
	w.closed = true
	runtime.SetFinalizer(w, nil)
}

// Reader is an io.ReadCloser that decompresses a stream produced by the
// given algorithm from an underlying source. Close releases the C stream.
//
// By default concatenated zstd/LZ4 frames, gzip members and xz streams decode
// as one stream. Multistream(false) instead stops at the end of the first one
// and hands back the bytes read past it through Unused.
//
// A Reader is not safe for concurrent use.
type Reader struct {
	src     io.Reader
	algo    Algorithm
	stream  *C.cu_decompress_stream_t
	inBuf   []byte       // scratch for reads from src
	scratch []byte       // decompressor output scratch
	out     bytes.Buffer // decompressed bytes not yet handed to Read

	srcEOF    bool // src has reported io.EOF
	inputDone bool // all input has been fed; in the finish phase
	draining  bool // C stream holds unconsumed input; feed (nil,0) next
	finished  bool // finish returned OK, or single mode reached eof; fully drained
	err       error
	closed    bool

	single    bool   // Multistream(false): stop at the end of the first stream
	started   bool   // Read has been called
	unused    []byte // single mode: bytes read from src past the end
	maxOutput uint64 // SetMaxOutput; 0 = no limit
}

// NewReader returns a Reader that decompresses src using the given
// algorithm.
func NewReader(src io.Reader, algo Algorithm) (*Reader, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var s *C.cu_decompress_stream_t
	st := C.cu_decompress_stream_create(C.cu_algorithm_t(algo), &s)
	if err := statusErr(st); err != nil {
		return nil, err
	}
	r := &Reader{
		src:     src,
		algo:    algo,
		stream:  s,
		inBuf:   make([]byte, streamScratch),
		scratch: make([]byte, streamScratch),
	}
	runtime.SetFinalizer(r, (*Reader).free)
	return r, nil
}

// Multistream controls whether the Reader decodes concatenated frames as one
// stream (true, the default) or stops at the end of the first zstd frame, LZ4
// frame, gzip member, zlib/xz/brotli/bzip2/raw DEFLATE stream or Snappy block
// (false), like compress/gzip's Reader.Multistream. In single-stream mode Read
// returns io.EOF at that end, even if more data follows, and Unused returns
// the bytes the Reader read from its source past it.
//
// It must be called before the first Read; calling it afterwards panics. If
// the algorithm can't decode a single stream (Lz4Raw has no end marker; Lzo),
// the first Read returns an *Error with Code ErrUnsupported.
func (r *Reader) Multistream(ok bool) {
	if r.started {
		panic("compressutils: Reader.Multistream called after Read")
	}
	if r.stream == nil {
		return // closed
	}
	r.err = nil
	if r.single == !ok {
		return
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var flags C.uint32_t
	if !ok {
		flags = C.CU_DSTREAM_SINGLE
	}
	var s *C.cu_decompress_stream_t
	st := C.cu_decompress_stream_create_ex(C.cu_algorithm_t(r.algo), flags, &s)
	if err := statusErr(st); err != nil {
		r.err = err
		return
	}
	C.cu_decompress_stream_destroy(r.stream)
	r.stream = s
	r.single = !ok
	C.cu_decompress_stream_set_max_output(r.stream, C.size_t(r.maxOutput))
}

// SetMaxOutput limits the total decompressed output to n bytes (0, the
// default, means no limit). The Read that would go past it returns the bytes
// up to the limit and then an *Error with Code ErrSizeLimit; the Reader is
// done after that. Use it when decompressing untrusted input, which can
// expand to far more than its own size.
func (r *Reader) SetMaxOutput(n uint64) {
	r.maxOutput = n
	if r.stream == nil {
		return // closed
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	C.cu_decompress_stream_set_max_output(r.stream, C.size_t(n))
}

// Unused returns, in single-stream mode (Multistream(false)) once Read has
// returned io.EOF, the bytes the Reader read from its source past the end of
// the stream, so the caller can carry on reading where the stream ended:
//
//	next := io.MultiReader(bytes.NewReader(r.Unused()), src)
//
// Unlike compress/gzip, which avoids reading past the end by pulling one byte
// at a time through an io.ByteReader, this Reader reads its source in chunks
// and hands the extra bytes back here. It returns nil before the end, in the
// default multistream mode, and when nothing followed the stream. The slice
// stays valid after Close.
func (r *Reader) Unused() []byte {
	return r.unused
}

// Read decompresses into p. It returns io.EOF once the compressed stream has
// been fully consumed and drained (in single-stream mode, once the first stream
// has ended).
func (r *Reader) Read(p []byte) (int, error) {
	r.started = true
	for r.out.Len() == 0 {
		if r.finished {
			return 0, io.EOF
		}
		if r.err != nil {
			return 0, r.err
		}
		if err := r.pump(); err != nil {
			r.err = err
			// Surface buffered output before the error, matching io semantics.
			if r.out.Len() == 0 {
				return 0, err
			}
			break
		}
	}
	return r.out.Read(p)
}

// pump advances the decompressor by one step, appending any produced output
// to r.out and updating the feed/drain/finish state machine.
func (r *Reader) pump() error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	// Finish phase: input exhausted, flush trailing output.
	if r.inputDone && !r.draining {
		outLen := C.size_t(len(r.scratch))
		st := C.cu_decompress_stream_finish(r.stream, bytePtr(r.scratch), &outLen)
		if n := int(outLen); n > 0 {
			r.out.Write(r.scratch[:n])
		}
		if st == C.CU_OK {
			r.finished = true
			r.takeUnused()
			return nil
		}
		if Status(st) == ErrBufTooSmall {
			return nil // more finish output on the next pump
		}
		return statusErr(st)
	}

	// Decide what to feed this step: nil while draining retained input,
	// otherwise a fresh chunk from src.
	var in []byte
	if !r.draining {
		n, err := r.src.Read(r.inBuf)
		if n > 0 {
			in = r.inBuf[:n]
		}
		switch {
		case err == io.EOF:
			r.srcEOF = true
		case err != nil:
			return err
		}
		if n == 0 && r.srcEOF {
			r.inputDone = true
			return nil // re-enter pump in the finish phase
		}
	}

	outLen := C.size_t(len(r.scratch))
	st := C.cu_decompress_stream_write(r.stream,
		bytePtr(in), C.size_t(len(in)), bytePtr(r.scratch), &outLen)
	if n := int(outLen); n > 0 {
		r.out.Write(r.scratch[:n])
	}
	switch Status(st) {
	case statusOK:
		r.draining = false
		if r.single && C.cu_decompress_stream_eof(r.stream) != 0 {
			// The stream ended: stop reading src.
			r.finished = true
			r.takeUnused()
			return nil
		}
		if r.srcEOF {
			// The final chunk was fully consumed; flush next.
			r.inputDone = true
		}
		return nil
	case ErrBufTooSmall:
		r.draining = true // unconsumed input remains; drain with (nil,0)
		return nil
	default:
		return statusErr(st)
	}
}

// takeUnused copies the C stream's unused input into Go memory (it's only valid
// until the next C call on the stream). Every chunk read from src is handed to
// the C stream whole, which keeps whatever it doesn't decode, so its unused
// input is everything read past the end; the Reader holds no unfed bytes of
// its own. The caller must hold the OS thread.
func (r *Reader) takeUnused() {
	if !r.single || C.cu_decompress_stream_eof(r.stream) == 0 {
		return
	}
	var data *C.uint8_t
	var n C.size_t
	if C.cu_decompress_stream_unused(r.stream, &data, &n) != C.CU_OK || n == 0 {
		return
	}
	r.unused = append([]byte(nil), unsafe.Slice((*byte)(unsafe.Pointer(data)), int(n))...)
}

// Close releases the underlying C stream. It is safe to call more than once.
func (r *Reader) Close() error {
	if r.closed {
		return nil
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	r.free()
	return nil
}

func (r *Reader) free() {
	if r.stream != nil {
		C.cu_decompress_stream_destroy(r.stream)
		r.stream = nil
	}
	r.closed = true
	runtime.SetFinalizer(r, nil)
}
