#!/usr/bin/env bash
#
# The ecosystem comparison, end to end, on this machine: compress-utils against the
# alternatives in every binding, at all ten compress-utils levels, each competitor at the
# native level ours maps to (levels.json). Five combinations: one-shot on dickens (10 MB of
# text), xml (repetitive) and x-ray (hard to compress); streaming on dickens; and a
# small-message tier (dickens in 4 KB blocks).
#
#   benchmarks/run_ecosystem.sh                    # every stage
#   benchmarks/run_ecosystem.sh c python           # just these stages
#   benchmarks/run_ecosystem.sh --list
#
# Stages: c, python, go, rust, js-node, js-bun. Needs the same setup as runner.py
# (benchmarks/README.md): ./build.sh for the C and Python bindings, deps.py for the C
# alternatives, WASM modules in bindings/wasm/dist, node and bun on PATH.
# Environment: LEVELS (default 1..10), BUN_LEVELS (default 1,6,10: Bun is the slow
# JavaScript stage), OUT (log directory, default benchmarks/results/logs).

cd "$(dirname "$0")/.." || exit 1

LEVELS=${LEVELS:-1,2,3,4,5,6,7,8,9,10}
BUN_LEVELS=${BUN_LEVELS:-1,6,10}
OUT=${OUT:-benchmarks/results/logs}
ALGOS=zstd,brotli,zlib,gzip,deflate,bz2,lz4,xz,snappy
ALGOS_NO_DEFLATE=zstd,brotli,zlib,gzip,bz2,lz4,xz,snappy

C=c,c-baseline,c-zlib-ng,c-libdeflate,c-miniz,c-isa-l,c-zlib-rs,c-libbz2-rs,c-lzma-sdk,c-google-snappy,c-libdeflate-percall,c-libzstd-ctx,c-liblzma-fitdict
PY=python,python-stdlib,python-cramjam,python-zstandard,python-brotli,python-lz4
GO=go,go-stdlib,go-klauspost,go-brotli,go-lz4,go-xz,go-bzip2
RS=rust,rust-flate2,rust-zstd,rust-brotli,rust-lz4_flex,rust-snap,rust-bzip2,rust-xz2,rust-flate2-zlib-rs
JS=wasm,js-compression-stream,js-fflate,js-pako,js-brotli-wasm,js-zstd-wasm,js-fzstd,js-xz-decompress,js-lz4js,js-snappyjs

if [ "$1" = "--list" ]; then echo "c python go rust js-node js-bun"; exit 0; fi
STAGES=${*:-"c python go rust js-node js-bun"}
mkdir -p "$OUT"

# run NAME DRIVERS ALGOS LEVELS [runner args]: the large files at 1 sample, the small
# messages at 3 (each sample is one pass over all 256 messages), one warmup each. The
# extra files run one-shot only.
run() {
    name=$1; drivers=$2; algos=$3; levels=$4; shift 4
    for part in "ecosystem-large 1 oneshot,stream" "ecosystem-extra 1 oneshot" "ecosystem-small 3 oneshot,stream"; do
        set -- $part "$@"; tier=$1; samples=$2; modes=$3; shift 3
        echo "[$(date +%H:%M)] $name: $tier" >&2
        python3 benchmarks/runner.py --drivers "$drivers" --algos "$algos" --levels "$levels" \
            --modes "$modes" --corpus "$tier" --samples "$samples" "$@" \
            > "$OUT/$name-$tier.log" 2>&1 || echo "[run_ecosystem] $name $tier FAILED, see $OUT/$name-$tier.log" >&2
    done
}

for stage in $STAGES; do
    case $stage in
        c)       run c "$C" $ALGOS "$LEVELS" ;;
        python)  run python "$PY" $ALGOS "$LEVELS" ;;
        go)      run go "$GO" $ALGOS_NO_DEFLATE "$LEVELS" ;;
        rust)    run rust "$RS" $ALGOS_NO_DEFLATE "$LEVELS" ;;
        js-node) run js-node "$JS" $ALGOS "$LEVELS" --js-runtime node ;;
        js-bun)  run js-bun "$JS" $ALGOS "$BUN_LEVELS" --js-runtime bun ;;
        *) echo "unknown stage '$stage' (try --list)" >&2; exit 2 ;;
    esac
done
echo "[$(date +%H:%M)] done" >&2
