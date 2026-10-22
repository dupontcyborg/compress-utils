// Go benchmark driver.
//
// Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
// "<algo> <level> [<mode>] <path>" job lines from stdin (mode: oneshot, stream or
// msgset), emits one NDJSON result
// (or skip/error marker) per line, honours BENCH_SAMPLES / BENCH_WARMUP /
// BENCH_CHUNK, and answers `--info`.
//
// Drives the compress-utils Go binding (bindings/go) the way a consumer would:
// Compress/Decompress for one-shot, NewWriter/NewReader for streaming. A message
// set (benchmarks/corpus/msgset.py) runs one Compress call per message and
// decompresses each with DecompressWithCapacity, its fastest path when the size
// is known. The
// binding compiles the C core from source via cgo, so this is the same codec
// code the other drivers measure — only the language wrapper differs.
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	cu "github.com/dupontcyborg/compress-utils/bindings/go"
)

var algos = map[string]cu.Algorithm{
	"zstd":   cu.Zstd,
	"brotli": cu.Brotli,
	"zlib":   cu.Zlib,
	"bz2":    cu.Bz2,
	"lz4":    cu.Lz4,
	"xz":     cu.Xz,
	"snappy": cu.Snappy,
	"gzip":   cu.Gzip,
}

func envInt(name string, fallback int) int {
	if v := os.Getenv(name); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			return n
		}
	}
	return fallback
}

var (
	samples = envInt("BENCH_SAMPLES", 5)
	warmup  = envInt("BENCH_WARMUP", 1)
	chunk   = envInt("BENCH_CHUNK", 64*1024)
)

type stats struct{ median, mad, min int64 }

func computeStats(xs []int64) stats {
	s := append([]int64(nil), xs...)
	sort.Slice(s, func(i, j int) bool { return s[i] < s[j] })
	n := len(s)
	med := func(v []int64) int64 {
		m := len(v)
		if m%2 == 1 {
			return v[m/2]
		}
		return (v[m/2-1] + v[m/2]) / 2
	}
	m := med(s)
	dev := make([]int64, n)
	for i, x := range s {
		if x > m {
			dev[i] = x - m
		} else {
			dev[i] = m - x
		}
	}
	sort.Slice(dev, func(i, j int) bool { return dev[i] < dev[j] })
	return stats{median: m, mad: med(dev), min: s[0]}
}

func compress(algo cu.Algorithm, data []byte, level int, isStream bool) ([]byte, error) {
	if !isStream {
		return cu.Compress(algo, data, level)
	}
	var buf bytes.Buffer
	w, err := cu.NewWriter(&buf, algo, level)
	if err != nil {
		return nil, err
	}
	for off := 0; off < len(data); off += chunk {
		end := off + chunk
		if end > len(data) {
			end = len(data)
		}
		if _, err := w.Write(data[off:end]); err != nil {
			w.Close()
			return nil, err
		}
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func decompress(algo cu.Algorithm, comp []byte, isStream bool) ([]byte, error) {
	if !isStream {
		return cu.Decompress(algo, comp)
	}
	r, err := cu.NewReader(bytes.NewReader(comp), algo)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

func runJob(algoName string, level int, isStream bool, path string) (map[string]any, error) {
	algo, ok := algos[algoName]
	if !ok {
		return nil, fmt.Errorf("unknown algo %q", algoName)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	var comp []byte
	for i := 0; i < warmup; i++ {
		if comp, err = compress(algo, data, level, isStream); err != nil {
			return nil, err
		}
	}
	cT := make([]int64, samples)
	for i := 0; i < samples; i++ {
		t0 := time.Now()
		if comp, err = compress(algo, data, level, isStream); err != nil {
			return nil, err
		}
		cT[i] = time.Since(t0).Nanoseconds()
	}

	var dec []byte
	for i := 0; i < warmup; i++ {
		if dec, err = decompress(algo, comp, isStream); err != nil {
			return nil, err
		}
	}
	dT := make([]int64, samples)
	for i := 0; i < samples; i++ {
		t0 := time.Now()
		if dec, err = decompress(algo, comp, isStream); err != nil {
			return nil, err
		}
		dT[i] = time.Since(t0).Nanoseconds()
	}

	c, d := computeStats(cT), computeStats(dT)
	mode := "oneshot"
	chunkBytes := 0
	if isStream {
		mode = "stream"
		chunkBytes = chunk
	}
	return map[string]any{
		"lang":                 "go",
		"impl":                 "compress-utils",
		"algo":                 algoName,
		"level":                level,
		"mode":                 mode,
		"chunk_bytes":          chunkBytes,
		"input":                path,
		"input_bytes":          len(data),
		"output_bytes":         len(comp),
		"compress_ns_median":   c.median,
		"compress_ns_mad":      c.mad,
		"compress_ns_min":      c.min,
		"decompress_ns_median": d.median,
		"decompress_ns_mad":    d.mad,
		"decompress_ns_min":    d.min,
		"samples":              samples,
		"warmup":               warmup,
		"verified":             bytes.Equal(dec, data),
	}, nil
}

// readMsgset reads a message set: records of a u32 little-endian length
// followed by that many bytes.
func readMsgset(path string) ([][]byte, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var msgs [][]byte
	for pos := 0; pos < len(data); {
		if len(data)-pos < 4 {
			return nil, errors.New("truncated message set")
		}
		n := int(binary.LittleEndian.Uint32(data[pos:]))
		pos += 4
		if n > len(data)-pos {
			return nil, errors.New("truncated message set")
		}
		msgs = append(msgs, data[pos:pos+n])
		pos += n
	}
	return msgs, nil
}

func runMsgsetJob(algoName string, level int, path string) (map[string]any, error) {
	algo := algos[algoName]
	msgs, err := readMsgset(path)
	if err != nil {
		return nil, err
	}

	comp := make([][]byte, len(msgs))
	cT := make([]int64, 0, samples)
	for r := 0; r < warmup+samples; r++ {
		t0 := time.Now()
		for i, m := range msgs {
			if comp[i], err = cu.Compress(algo, m, level); err != nil {
				return nil, err
			}
		}
		if r >= warmup {
			cT = append(cT, time.Since(t0).Nanoseconds())
		}
	}

	dT := make([]int64, 0, samples)
	for r := 0; r < warmup+samples; r++ {
		t0 := time.Now()
		for i, m := range msgs {
			if _, err = cu.DecompressWithCapacity(algo, comp[i], len(m)); err != nil {
				return nil, err
			}
		}
		if r >= warmup {
			dT = append(dT, time.Since(t0).Nanoseconds())
		}
	}

	verified := true
	inBytes, outBytes := 0, 0
	for i, m := range msgs {
		dec, err := cu.DecompressWithCapacity(algo, comp[i], len(m))
		verified = verified && err == nil && bytes.Equal(dec, m)
		inBytes += len(m)
		outBytes += len(comp[i])
	}

	c, d := computeStats(cT), computeStats(dT)
	return map[string]any{
		"lang":                  "go",
		"impl":                  "compress-utils",
		"algo":                  algoName,
		"level":                 level,
		"mode":                  "msgset",
		"chunk_bytes":           0,
		"input":                 path,
		"input_bytes":           inBytes,
		"output_bytes":          outBytes,
		"messages":              len(msgs),
		"decompress_size_known": true,
		"compress_ns_median":    c.median,
		"compress_ns_mad":       c.mad,
		"compress_ns_min":       c.min,
		"decompress_ns_median":  d.median,
		"decompress_ns_mad":     d.mad,
		"decompress_ns_min":     d.min,
		"samples":               samples,
		"warmup":                warmup,
		"verified":              verified,
	}, nil
}

func emit(obj map[string]any) {
	b, _ := json.Marshal(obj)
	fmt.Println(string(b))
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--info" {
		emit(map[string]any{"lang": "go", "version": cu.Version(), "driver": "go"})
		return
	}

	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		// "<algo> <level> [<mode>] <path>"; path may contain spaces.
		f := strings.SplitN(line, " ", 3)
		if len(f) < 3 {
			emit(map[string]any{"error": true})
			continue
		}
		algoName, levelS, rest := f[0], f[1], f[2]
		mode := "oneshot"
		for _, m := range []string{"stream", "msgset", "oneshot"} {
			if strings.HasPrefix(rest, m+" ") {
				mode, rest = m, rest[len(m)+1:]
				break
			}
		}
		path := strings.TrimSpace(rest)

		if _, ok := algos[algoName]; !ok {
			emit(map[string]any{"skipped": true})
			continue
		}
		level, err := strconv.Atoi(levelS)
		if err != nil {
			emit(map[string]any{"error": true})
			continue
		}
		var rec map[string]any
		if mode == "msgset" {
			rec, err = runMsgsetJob(algoName, level, path)
		} else {
			rec, err = runJob(algoName, level, mode == "stream", path)
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "bench-go: %s L%d %s failed: %v\n", algoName, level, mode, err)
			emit(map[string]any{"error": true})
			continue
		}
		emit(rec)
	}
}
