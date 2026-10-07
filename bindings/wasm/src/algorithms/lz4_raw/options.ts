/** Validates the caller-supplied capacity required by raw LZ4 blocks. */
import { CompressError, Status, type DecompressOptions } from "../../core/types.js";

export function validateDecompressOptions(options: DecompressOptions): void {
    const capacity = options.expectedSize;
    if (
        capacity === undefined ||
        !Number.isSafeInteger(capacity) ||
        capacity < 0 ||
        capacity > 0x7fffffff
    ) {
        throw new CompressError(
            Status.InvalidArg,
            "lz4_raw",
            "raw LZ4 decompression requires expectedSize (output capacity, integer 0..2147483647)",
        );
    }
}
