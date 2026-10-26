// Go benchmark driver, for compress-utils and for the Go libraries it competes with.
//
// Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
// "<algo> <level> [<mode>] <path>" job lines from stdin (mode: oneshot, stream or
// msgset), emits one NDJSON result (or skip/error marker) per line, honours
// BENCH_SAMPLES / BENCH_WARMUP / BENCH_CHUNK, and answers `--info`.
//
// `--impl` picks the implementation (default compress-utils); every one goes through
// the same timing loop, as a consumer would call it (impls.go):
//
//	oneshot  compress, then decompress without being told the size
//	msgset   one compress call per message, each decompressed with its known size
//	         through the library's fastest path for that
//	stream   the input fed in BENCH_CHUNK pieces through the library's streaming API;
//	         skipped when it has none
//
// Competitors run at native levels (the runner translates through levels.json, or
// sweeps the range each codec declares) and are set to compress-utils' wire format
// where their API allows. Each competitor's output is also decoded by compress-utils
// once per job, untimed, and the record carries the result as `interop`. compress-utils
// compiles its C core from source via cgo, so it runs the same codec code as the other
// drivers; only the language wrapper differs.
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
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

func goVersion() string { return runtime.Version() }

// modVersion is the version of a dependency, as linked into this binary.
func modVersion(path string) string {
	if bi, ok := debug.ReadBuildInfo(); ok {
		for _, d := range bi.Deps {
			if d.Path == path {
				return d.Version
			}
		}
	}
	return "unknown"
}

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

// timed runs fn warmup+samples times and returns the last result and the timed runs.
func timed[T any](fn func() (T, error)) (T, []int64, error) {
	var out T
	var err error
	times := make([]int64, 0, samples)
	for r := 0; r < warmup+samples; r++ {
		t0 := time.Now()
		if out, err = fn(); err != nil {
			return out, nil, err
		}
		if r >= warmup {
			times = append(times, time.Since(t0).Nanoseconds())
		}
	}
	return out, times, nil
}

type job struct {
	implName string
	algoName string
	codec    Codec
	level    int
}

func (j job) record(mode, path string, inBytes, outBytes int, cT, dT []int64, verified bool,
	comp0, data0 []byte, extra map[string]any) map[string]any {
	c, d := computeStats(cT), computeStats(dT)
	chunkBytes := 0
	if mode == "stream" {
		chunkBytes = chunk
	}
	rec := map[string]any{
		"lang": "go", "impl": j.implName, "algo": j.algoName, "level": j.level, "mode": mode,
		"chunk_bytes": chunkBytes, "input": path, "input_bytes": inBytes, "output_bytes": outBytes,
		"compress_ns_median": c.median, "compress_ns_mad": c.mad, "compress_ns_min": c.min,
		"decompress_ns_median": d.median, "decompress_ns_mad": d.mad, "decompress_ns_min": d.min,
		"samples": samples, "warmup": warmup, "verified": verified,
	}
	for k, v := range extra {
		rec[k] = v
	}
	if j.implName != "compress-utils" && comp0 != nil {
		out, err := cu.Decompress(algos[j.algoName], comp0)
		rec["interop"] = err == nil && bytes.Equal(out, data0)
	}
	if j.codec.Compress == nil {
		rec["decode_only"] = true
	}
	return rec
}

func (j job) run(mode, path string) (map[string]any, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	stream := mode == "stream"

	var comp []byte
	cT := make([]int64, samples)
	if j.codec.Compress == nil {
		// Decode-only library: its input comes from compress-utils, untimed.
		if comp, err = cu.Compress(algos[j.algoName], data, 9); err != nil {
			return nil, err
		}
	} else {
		enc := j.codec.Compress
		if stream {
			enc = j.codec.CStream
		}
		if comp, cT, err = timed(func() ([]byte, error) { return enc(data, j.level) }); err != nil {
			return nil, err
		}
	}
	dec := func() ([]byte, error) { return j.codec.Decompress(comp, -1) }
	if stream {
		dec = func() ([]byte, error) { return j.codec.DStream(comp) }
	}
	out, dT, err := timed(dec)
	if err != nil {
		return nil, err
	}
	return j.record(mode, path, len(data), len(comp), cT, dT, bytes.Equal(out, data), comp, data, nil), nil
}

// readMsgset reads a message set: records of a u32 little-endian length followed by
// that many bytes.
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

func (j job) runMsgset(path string) (map[string]any, error) {
	msgs, err := readMsgset(path)
	if err != nil {
		return nil, err
	}
	comp := make([][]byte, len(msgs))
	cT := make([]int64, samples)
	if j.codec.Compress == nil {
		for i, m := range msgs {
			if comp[i], err = cu.Compress(algos[j.algoName], m, 9); err != nil {
				return nil, err
			}
		}
	} else {
		_, cT, err = timed(func() (struct{}, error) {
			for i, m := range msgs {
				c, err := j.codec.Compress(m, j.level)
				if err != nil {
					return struct{}{}, err
				}
				comp[i] = c
			}
			return struct{}{}, nil
		})
		if err != nil {
			return nil, err
		}
	}
	_, dT, err := timed(func() (struct{}, error) {
		for i, m := range msgs {
			if _, err := j.codec.Decompress(comp[i], len(m)); err != nil {
				return struct{}{}, err
			}
		}
		return struct{}{}, nil
	})
	if err != nil {
		return nil, err
	}

	verified := true
	inBytes, outBytes := 0, 0
	for i, m := range msgs {
		dec, err := j.codec.Decompress(comp[i], len(m))
		verified = verified && err == nil && bytes.Equal(dec, m)
		inBytes += len(m)
		outBytes += len(comp[i])
	}
	var c0, d0 []byte
	if len(msgs) > 0 {
		c0, d0 = comp[0], msgs[0]
	}
	return j.record("msgset", path, inBytes, outBytes, cT, dT, verified, c0, d0,
		map[string]any{"messages": len(msgs), "decompress_size_known": true}), nil
}

func emit(obj map[string]any) {
	b, _ := json.Marshal(obj)
	fmt.Println(string(b))
}

func main() {
	implName := flag.String("impl", "compress-utils", "implementation: "+strings.Join(implNames, ", "))
	info := flag.Bool("info", false, "print driver info and exit")
	flag.Parse()
	im, ok := loadImpl(*implName)
	if !ok {
		fmt.Fprintf(os.Stderr, "bench-go: unknown impl %q\n", *implName)
		os.Exit(2)
	}

	if *info {
		levels := map[string][2]int{}
		var levelless []string
		for a, c := range im.codecs {
			if c.Levels != nil {
				levels[a] = *c.Levels
			}
			if c.Levelless {
				levelless = append(levelless, a)
			}
		}
		sort.Strings(levelless)
		driver := "go"
		if *implName != "compress-utils" {
			driver = "go-" + *implName
		}
		emit(map[string]any{"lang": "go", "version": im.version, "driver": driver, "impl": *implName,
			"levels": levels, "levelless": levelless})
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

		codec, ok := im.codecs[algoName]
		if !ok || (mode == "stream" && (codec.DStream == nil || (codec.CStream == nil && codec.Compress != nil))) {
			emit(map[string]any{"skipped": true})
			continue
		}
		level, err := strconv.Atoi(levelS)
		if err != nil {
			emit(map[string]any{"error": true})
			continue
		}
		j := job{implName: *implName, algoName: algoName, codec: codec, level: level}
		var rec map[string]any
		if mode == "msgset" {
			rec, err = j.runMsgset(path)
		} else {
			rec, err = j.run(mode, path)
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "bench-go[%s]: %s L%d %s failed: %v\n", *implName, algoName, level, mode, err)
			emit(map[string]any{"error": true})
			continue
		}
		emit(rec)
	}
}
