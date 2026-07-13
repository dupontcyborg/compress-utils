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
// A Reader is not safe for concurrent use.
type Reader struct {
	src     io.Reader
	stream  *C.cu_decompress_stream_t
	inBuf   []byte       // scratch for reads from src
	scratch []byte       // decompressor output scratch
	out     bytes.Buffer // decompressed bytes not yet handed to Read

	srcEOF    bool // src has reported io.EOF
	inputDone bool // all input has been fed; in the finish phase
	draining  bool // C stream holds unconsumed input; feed (nil,0) next
	finished  bool // finish returned OK; fully drained
	err       error
	closed    bool
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
		stream:  s,
		inBuf:   make([]byte, streamScratch),
		scratch: make([]byte, streamScratch),
	}
	runtime.SetFinalizer(r, (*Reader).free)
	return r, nil
}

// Read decompresses into p. It returns io.EOF once the compressed stream has
// been fully consumed and drained.
func (r *Reader) Read(p []byte) (int, error) {
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
