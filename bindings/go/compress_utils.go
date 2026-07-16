package compressutils

/*
// The compress-utils C core and all eight codecs are compiled from source,
// straight from the vendored third_party/ tree — no prebuilt library, no
// network, so `go get` works with only a C compiler. cgo can only compile C
// that lives in this package directory, so tools/gen-go-cgo.py emits thin shim
// translation units (cu_gen_*.c/.cc) here that #include the real sources; the
// include dirs and INCLUDE_<ALGO> flags are in cgo_generated.go. Both are
// generated + committed; regenerate with tools/gen-go-cgo.py.
//
// Runtime link flags only (kept here, not generated, because they are platform
// runtime deps rather than manifest-derived). The library is pure C now — snappy
// switched from google/snappy (C++) to the andikleen C port, so NO C++ standard
// library is needed. The only remaining dep is libm on glibc (brotli/xz/zlib
// pull log2/pow/etc.); macOS bundles it in libSystem and Windows in the CRT.
#cgo linux LDFLAGS: -lm

#include <stdlib.h>
#include "compress_utils.h"
*/
import "C"

import (
	"bytes"
	"fmt"
	"io"
	"runtime"
	"unsafe"
)

// DefaultLevel is a sensible middle-of-the-road level, matching the C++ and
// Python bindings. Level runs 1 (fastest) .. 10 (smallest).
const DefaultLevel = 5

// Algorithm identifies a compression codec. The values match the CU_ALGO_*
// constants in the C ABI.
type Algorithm int

const (
	Zstd   Algorithm = C.CU_ALGO_ZSTD
	Brotli Algorithm = C.CU_ALGO_BROTLI
	Zlib   Algorithm = C.CU_ALGO_ZLIB
	Bz2    Algorithm = C.CU_ALGO_BZ2
	Lz4    Algorithm = C.CU_ALGO_LZ4
	Xz     Algorithm = C.CU_ALGO_XZ
	Lzma   Algorithm = C.CU_ALGO_LZMA // alias for Xz; produces .xz frames
	Snappy Algorithm = C.CU_ALGO_SNAPPY
	Gzip   Algorithm = C.CU_ALGO_GZIP
)

// Name returns the lowercase canonical name ("zstd", "brotli", ...), or ""
// for an unknown value.
func (a Algorithm) Name() string {
	p := C.cu_algorithm_name(C.cu_algorithm_t(a))
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

func (a Algorithm) String() string {
	if n := a.Name(); n != "" {
		return n
	}
	return fmt.Sprintf("Algorithm(%d)", int(a))
}

// Status is a C ABI status code. It is carried on Error and can be compared
// against the Err* constants below.
type Status int

const (
	statusOK          Status = C.CU_OK
	ErrInvalidArg     Status = C.CU_ERR_INVALID_ARG
	ErrInvalidLevel   Status = C.CU_ERR_INVALID_LEVEL
	ErrBufTooSmall    Status = C.CU_ERR_BUF_TOO_SMALL
	ErrSizeUnknown    Status = C.CU_ERR_SIZE_UNKNOWN
	ErrUnsupported    Status = C.CU_ERR_UNSUPPORTED_ALGO
	ErrCompression    Status = C.CU_ERR_COMPRESSION
	ErrDecompression  Status = C.CU_ERR_DECOMPRESSION
	ErrTruncated      Status = C.CU_ERR_TRUNCATED
	ErrSizeLimit      Status = C.CU_ERR_SIZE_LIMIT
	ErrStreamFinished Status = C.CU_ERR_STREAM_FINISHED
	ErrStreamState    Status = C.CU_ERR_STREAM_STATE
	ErrOOM            Status = C.CU_ERR_OOM
	ErrInternal       Status = C.CU_ERR_INTERNAL
)

// Error wraps a non-OK C status code together with the human-readable
// message the library reported.
type Error struct {
	Code Status
	Msg  string
}

func (e *Error) Error() string { return e.Msg }

// statusErr builds an Error for a non-OK status. It reads cu_last_error(),
// which is thread-local, so every caller must invoke it while the goroutine
// is pinned to the OS thread that made the failing call (see lockThread).
func statusErr(code C.cu_status_t) error {
	if code == C.CU_OK {
		return nil
	}
	msg := C.GoString(C.cu_strerror(code))
	if d := C.cu_last_error(); d != nil {
		if detail := C.GoString(d); detail != "" {
			msg += ": " + detail
		}
	}
	return &Error{Code: Status(code), Msg: msg}
}

// emptyByte backs the pointer bytePtr hands out for zero-length slices. cgo
// can't take &b[0] of an empty slice, and passing a NULL pointer (even with
// length 0) trips codecs that reject a NULL source outright — bzip2's
// BZ2_bzBuffToBuffCompress returns a param error. A pointer to this one-byte
// global is non-NULL and valid; the C side reads zero bytes, so it's untouched.
var emptyByte [1]byte

// bytePtr returns a *C.uint8_t to the first byte of b, or a non-NULL sentinel
// for an empty slice (paired with an explicit length of 0 at every call site).
func bytePtr(b []byte) *C.uint8_t {
	if len(b) == 0 {
		return (*C.uint8_t)(unsafe.Pointer(&emptyByte[0]))
	}
	return (*C.uint8_t)(unsafe.Pointer(&b[0]))
}

// Version returns the library version string ("MAJOR.MINOR.PATCH").
func Version() string {
	return C.GoString(C.cu_version())
}

// Available reports whether the algorithm was compiled into the linked
// library.
func Available(a Algorithm) bool {
	return C.cu_algorithm_available(C.cu_algorithm_t(a)) != 0
}

// SetMaxDecompressedSize sets a global cap on the output size one-shot
// Decompress will accept. Defaults to 1 GiB; 0 disables the cap. Does not
// affect the streaming Reader, which is expected to enforce its own limit.
func SetMaxDecompressedSize(bytes uint64) {
	C.cu_set_max_decompressed_size(C.size_t(bytes))
}

// Compress compresses data with the given algorithm at the given level
// (1 fastest .. 10 smallest) and returns the compressed bytes.
func Compress(algo Algorithm, data []byte, level int) ([]byte, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	bound := C.cu_compress_bound(C.size_t(len(data)), C.cu_algorithm_t(algo))
	out := make([]byte, int(bound))
	outLen := C.size_t(len(out))
	st := C.cu_compress(
		C.cu_algorithm_t(algo),
		bytePtr(data), C.size_t(len(data)),
		bytePtr(out), &outLen,
		C.int(level),
	)
	if err := statusErr(st); err != nil {
		return nil, err
	}
	return out[:int(outLen)], nil
}

// Decompress decompresses data produced by the given algorithm. When the
// wire format records the decompressed size it is recovered in a single
// pass; otherwise Decompress falls back to the streaming decoder.
func Decompress(algo Algorithm, data []byte) ([]byte, error) {
	out, unknownSize, err := decompressOneShot(algo, data)
	if err != nil {
		return nil, err
	}
	if !unknownSize {
		return out, nil
	}

	// Wire format does not carry the size (zlib, gzip, brotli, bz2, raw lz4):
	// stream-decompress the whole buffer.
	r, err := NewReader(bytes.NewReader(data), algo)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

// decompressOneShot attempts the size-hint + one-shot path. It returns
// unknownSize=true (with a nil slice and nil error) when the caller should
// fall back to streaming.
func decompressOneShot(algo Algorithm, data []byte) (out []byte, unknownSize bool, err error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	var hint C.size_t
	st := C.cu_decompress_size_hint(
		C.cu_algorithm_t(algo), bytePtr(data), C.size_t(len(data)), &hint)
	switch st {
	case C.CU_OK:
		out = make([]byte, int(hint))
		outLen := hint
		st2 := C.cu_decompress(
			C.cu_algorithm_t(algo), bytePtr(data), C.size_t(len(data)),
			bytePtr(out), &outLen)
		switch st2 {
		case C.CU_OK:
			return out[:int(outLen)], false, nil
		case C.CU_ERR_SIZE_UNKNOWN, C.CU_ERR_BUF_TOO_SMALL:
			return nil, true, nil
		default:
			return nil, false, statusErr(st2)
		}
	case C.CU_ERR_SIZE_UNKNOWN:
		return nil, true, nil
	default:
		return nil, false, statusErr(st)
	}
}
