/**
 * Public types. The Algorithm enum is internal — users see string names
 * via CompressError.algorithm. Each subpath knows its own algorithm,
 * so users never need to pass one.
 */

/** @internal */
export const enum Algorithm {
    Zstd = 0,
    Brotli = 1,
    Zlib = 2,
    Bz2 = 3,
    Lz4 = 4,
    Xz = 5,
    Lzma = 6,
    Snappy = 7,
    Gzip = 8,
    Deflate = 9,
    Lz4Raw = 10,
    Lzo = 11,
}

/** @internal */
export const enum Status {
    Ok = 0,
    InvalidArg = 1,
    InvalidLevel = 2,
    BufTooSmall = 3,
    SizeUnknown = 4,
    UnsupportedAlgo = 5,
    Compression = 6,
    Decompression = 7,
    Truncated = 8,
    SizeLimit = 9,
    StreamFinished = 10,
    StreamState = 11,
    Oom = 12,
    Internal = 13,
}

/** Canonical lower-case algorithm names. */
export type AlgorithmName =
    | "zstd"
    | "brotli"
    | "zlib"
    | "bz2"
    | "lz4"
    | "xz"
    | "snappy"
    | "gzip"
    | "deflate"
    | "lz4_raw"
    | "lzo";

/** Thrown for any non-Ok status. Carries the raw status code and algo name. */
export class CompressError extends Error {
    /** Underlying C status code. Stable across versions; see compress_utils.h. */
    readonly code: number;
    readonly algorithm: AlgorithmName;

    constructor(code: number, algorithm: AlgorithmName, message: string) {
        super(message);
        this.name = "CompressError";
        this.code = code;
        this.algorithm = algorithm;
    }
}

export interface CompressOptions {
    /** 1..10. 1 = fastest, 10 = smallest. Default 5. */
    level?: number;
}

export interface CompressionStreamOptions extends CompressOptions {
    /**
     * Flush after every chunk, so each output chunk is decodable on arrival
     * while the stream stays open: for request/response or message streams
     * such as a WebSocket. Costs a few bytes per chunk. Not supported by
     * snappy and bz2, which can't flush mid-stream.
     */
    flushEachChunk?: boolean;
}

export interface DecompressStreamOptions {
    /**
     * Stop at the end of the first frame/member/stream instead of decoding
     * concatenated frames as one stream (the default). Once `eof` is true,
     * `unusedData` holds the input bytes written after the end, so the caller
     * can read whatever follows (another message, a container's own bytes).
     * A write with data after the end throws `CompressError` (code
     * `StreamFinished`, 10); empty writes and `finish()` return empty output.
     * Snappy emits nothing until its block is complete. Not supported by
     * raw LZ4 and LZO (throws `UnsupportedAlgo`, 5).
     */
    singleStream?: boolean;
    /**
     * Limit on the total decompressed output in bytes (default: no limit).
     * The write or finish that would go past it throws `CompressError` (code
     * `SizeLimit`, 9), and the stream can't be used after that. Set it when
     * decompressing untrusted input, which can expand to far more than its
     * own size.
     */
    maxOutput?: number;
}

export interface DecompressOptions {
    /**
     * Decompressed size in bytes if known by the caller. Skips the
     * size-hint probe and any internal streaming fallback — a small
     * win for hot paths where the size is tracked out-of-band.
     */
    expectedSize?: number;
}
