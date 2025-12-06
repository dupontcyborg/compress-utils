// Mock Emscripten module factory for tree-shaking tests
export default async function Module() {
  return {
    HEAPU8: new Uint8Array(1024),
    wasmMemory: { buffer: new ArrayBuffer(1024) },
    _malloc: () => 0,
    _free: () => {},
    _cu_compress: () => 0,
    _cu_decompress: () => 0,
    _cu_stream_compress_create: () => 0,
    _cu_stream_compress_write: () => 0,
    _cu_stream_compress_finish: () => 0,
    _cu_stream_compress_destroy: () => {},
    _cu_stream_decompress_create: () => 0,
    _cu_stream_decompress_write: () => 0,
    _cu_stream_decompress_finish: () => 0,
    _cu_stream_decompress_destroy: () => {},
  };
}
