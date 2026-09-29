import native from 'libsteamdecompress.so';
import { hilog } from '../service/LogService';

const DOMAIN = 0x0000;
const TAG = 'SteamChunkDecoder';

/**
 * Depot chunk 解码 (W6): 识别魔数 → 剥壳 → 调 libsteamdecompress.so。
 * 魔数 (SteamKit DepotChunk): 'VSZa'=zstd, 'VZa'=VZip(LZMA), 裸 LZMA (VZip 旧变体无壳) 兜底。
 */
export class SteamChunkDecoder {
  /** 解压一个 chunk; 输出尺寸来自 manifest 的 uncompressedSize。 */
  static decode(payload: Uint8Array, uncompressedSize: number): Uint8Array {
    if (payload.length >= 4 && payload[0] === 0x56 && payload[1] === 0x53
      && payload[2] === 0x5A && payload[3] === 0x61) {
      // 'VSZa' → zstd 帧 (壳头 4B + 帧 + 尾 'zsv' 3B + crc 4B + 原长 8B = 尾 15B)
      const body = payload.subarray(4, payload.length - 15);
      return SteamChunkDecoder.runNative(body, uncompressedSize, 'zstd');
    }
    if (payload.length >= 3 && payload[0] === 0x56 && payload[1] === 0x5A
      && payload[2] === 0x61) {
      // 'VZa' → VZip 容器: 头 7B ('VZ'+'a'+ts4B) + 5B props + LZMA 流 + 尾 14B ('zv'+crc4B+原长8B)
      const body = payload.subarray(7, payload.length - 14);
      return SteamChunkDecoder.runNative(body, uncompressedSize, 'lzma');
    }
    // 无壳: 直接是 5B props + LZMA 流
    return SteamChunkDecoder.runNative(payload, uncompressedSize, 'lzma');
  }

  private static runNative(body: Uint8Array, expectedSize: number, format: string): Uint8Array {
    const copy = new Uint8Array(body.length);
    copy.set(body);
    const result = native.decompressChunk(copy, expectedSize, format) as ArrayBuffer;
    const out = new Uint8Array(result);
    if (out.length !== expectedSize) {
      hilog.warn(DOMAIN, TAG, 'chunk size mismatch: %{public}d != %{public}d', out.length, expectedSize);
      throw new Error(`解压尺寸不符 (${out.length} != ${expectedSize})`);
    }
    return out;
  }
}
