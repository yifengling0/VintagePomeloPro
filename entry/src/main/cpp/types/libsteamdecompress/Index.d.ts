/**
 * Steam depot chunk 解压 (W6)。
 * format: "lzma" = 剥掉 VZip 壳后的 5B props + LZMA 流;
 *         "zstd" = 剥掉 VSZa 壳后的 zstd 帧。
 * 返回解压后的 ArrayBuffer。
 */
export const decompressChunk: (bytes: Uint8Array, expectedSize: number, format: string) => ArrayBuffer;
export const unzipManifest: (bytes: Uint8Array) => ArrayBuffer;
