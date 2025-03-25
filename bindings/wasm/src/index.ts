/**
 * Compress-Utils JavaScript/TypeScript API
 */
import type { Algorithm, CompressUtilsModule } from './compress_utils';
import CompressUtilsFactory from './compress_utils';

// Module singleton
let modulePromise: Promise<CompressUtilsModule> | null = null;

/**
 * Initialize the WebAssembly module
 * @returns Promise that resolves with the loaded module
 */
async function getModule(): Promise<CompressUtilsModule> {
  if (!modulePromise) {
    modulePromise = CompressUtilsFactory().catch((error: Error): never => {
      modulePromise = null;
      throw new Error(`Failed to initialize compress-utils: ${error.message}`);
    });
  }
  return modulePromise;
}

/**
 * Get the list of available compression algorithms
 * @returns Array of available algorithm names
 */
export async function getAvailableAlgorithms(): Promise<Algorithm[]> {
  const module = await getModule();
  return module.getAvailableAlgorithms();
}

/**
 * Compress data using the specified algorithm
 * @param data Data to compress
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1=fastest, 9=best compression, default=3)
 * @returns Compressed data as Uint8Array
 * @throws {Error} If compression fails
 */
export async function compress(
  data: Uint8Array | ArrayBuffer | number[],
  algorithm: Algorithm,
  level = 3
): Promise<Uint8Array> {
  const module = await getModule();
  
  // Convert ArrayBuffer to Uint8Array if needed
  if (data instanceof ArrayBuffer) {
    data = new Uint8Array(data);
  }
  
  return module.compress(data, algorithm, level);
}

/**
 * Decompress data using the specified algorithm
 * @param data Data to decompress
 * @param algorithm Compression algorithm to use
 * @returns Decompressed data as Uint8Array
 * @throws {Error} If decompression fails
 */
export async function decompress(
  data: Uint8Array | ArrayBuffer | number[],
  algorithm: Algorithm
): Promise<Uint8Array> {
  const module = await getModule();
  
  // Convert ArrayBuffer to Uint8Array if needed
  if (data instanceof ArrayBuffer) {
    data = new Uint8Array(data);
  }
  
  return module.decompress(data, algorithm);
}

/**
 * OOP interface for compression with the same algorithm
 */
export class Compressor {
  private algorithm: Algorithm;
  private compressorPromise: Promise<InstanceType<CompressUtilsModule['Compressor']>>;

  /**
   * Create a new Compressor instance
   * @param algorithm The compression algorithm to use
   */
  constructor(algorithm: Algorithm) {
    this.algorithm = algorithm;
    this.compressorPromise = getModule().then(module => 
      new module.Compressor(algorithm)
    );
  }

  /**
   * Compress data
   * @param data Data to compress
   * @param level Compression level (1=fastest, 9=best compression, default=3)
   * @returns Compressed data as Uint8Array
   */
  async compress(
    data: Uint8Array | ArrayBuffer | number[],
    level = 3
  ): Promise<Uint8Array> {
    const compressor = await this.compressorPromise;
    
    // Convert ArrayBuffer to Uint8Array if needed
    if (data instanceof ArrayBuffer) {
      data = new Uint8Array(data);
    }
    
    return compressor.compress(data, level);
  }

  /**
   * Decompress data
   * @param data Data to decompress
   * @returns Decompressed data as Uint8Array
   */
  async decompress(
    data: Uint8Array | ArrayBuffer | number[]
  ): Promise<Uint8Array> {
    const compressor = await this.compressorPromise;
    
    // Convert ArrayBuffer to Uint8Array if needed
    if (data instanceof ArrayBuffer) {
      data = new Uint8Array(data);
    }
    
    return compressor.decompress(data);
  }
}

// Re-export the Algorithm type
export type { Algorithm };