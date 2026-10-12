/** Validates the caller-supplied output capacity that raw block formats require. */
import { CompressError, Status, type AlgorithmName, type DecompressOptions } from "./types.js";

export function validateCapacity(
    name: AlgorithmName,
    format: string,
    options: DecompressOptions | undefined,
): void {
    const capacity = options?.expectedSize;
    if (
        capacity === undefined ||
        !Number.isSafeInteger(capacity) ||
        capacity < 0 ||
        capacity > 0x7fffffff
    ) {
        throw new CompressError(
            Status.InvalidArg,
            name,
            `${format} decompression requires expectedSize (output capacity, integer 0..2147483647)`,
        );
    }
}
