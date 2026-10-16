/** Validates an optional caller-supplied output capacity for raw block formats. */
import { CompressError, Status, type AlgorithmName, type DecompressOptions } from "./types.js";

export function validateCapacity(
    name: AlgorithmName,
    format: string,
    options: DecompressOptions | undefined,
): void {
    const capacity = options?.expectedSize;
    if (capacity === undefined) return;
    if (!Number.isSafeInteger(capacity) || capacity < 0 || capacity > 0x7fffffff) {
        throw new CompressError(
            Status.InvalidArg,
            name,
            `${format} expectedSize must be an integer 0..2147483647 (output capacity)`,
        );
    }
}
