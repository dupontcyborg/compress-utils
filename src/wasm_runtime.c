/*
 * wasm_runtime.c — host-callable allocator shims for the WASM build.
 *
 * Only compiled into the wasm32-* targets. The JS dispatcher needs a way
 * to allocate buffers inside the module's linear memory so it can stage
 * input bytes before calling cu_compress / cu_decompress. We deliberately
 * do NOT export the libc names `malloc` / `free` — exposing them locks us
 * to a specific allocator and pollutes the ABI. `cu_alloc` / `cu_free`
 * are part of the WASM-side contract documented in
 * bindings/wasm/src/core/dispatch.ts.
 */

#ifdef __wasm__

#include <stdlib.h>

__attribute__((export_name("cu_alloc")))
void* cu_alloc(size_t bytes) {
    return malloc(bytes);
}

__attribute__((export_name("cu_free")))
void cu_free(void* ptr) {
    free(ptr);
}

#endif /* __wasm__ */
