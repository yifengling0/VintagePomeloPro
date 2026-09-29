import zlib from '@ohos.zlib';
import { IClock } from './vendor/contracts/SteamPlatformContracts';
import { ISteamCmScheduler } from './vendor/contracts/SteamCmContracts';
import { ISteamCmMultiDecompressor } from './vendor/protocol/SteamCmProtocol';

/** HarmonyOS timer 实现（steam_core 调度缝隙）。 */
export class SteamCmScheduler implements ISteamCmScheduler {
  schedule(delayMilliseconds: number, callback: () => void): number {
    return setTimeout(callback, Math.max(0, Math.floor(delayMilliseconds)));
  }

  cancel(handle: number): void {
    clearTimeout(handle);
  }
}

/** 墙钟。 */
export class SteamHostClock implements IClock {
  nowEpochSeconds(): number {
    return Math.floor(Date.now() / 1000);
  }
}

const MAX_INFLATE_ITERATIONS = 8192;
const GZIP_WINDOW_BITS = 16;

/** CMsgMulti 的 gzip 解压器（@ohos.zlib 同步流接口，对齐 SteamKit2 gzip 模式）。 */
export class SteamCmMultiDecompressor implements ISteamCmMultiDecompressor {
  async decompress(payload: Uint8Array, expectedSize: number): Promise<Uint8Array> {
    if (!Number.isSafeInteger(expectedSize) || expectedSize <= 0 || expectedSize > 67108864) {
      throw new Error('Steam CM compressed Multi size is invalid: ' + expectedSize);
    }
    const inputBuffer = new ArrayBuffer(payload.length);
    new Uint8Array(inputBuffer).set(payload);
    const outputBuffer = new ArrayBuffer(expectedSize);
    const zip = zlib.createZipSync();
    const stream: zlib.ZStream = {
      nextIn: inputBuffer,
      availableIn: payload.length,
      nextOut: outputBuffer,
      availableOut: expectedSize
    };
    try {
      const initStatus = await zip.inflateInit2(stream, GZIP_WINDOW_BITS);
      if (initStatus !== zlib.ReturnStatus.OK) {
        throw new Error('gzip inflateInit2 failed: ' + initStatus);
      }
      for (let iteration = 0; iteration < MAX_INFLATE_ITERATIONS; iteration++) {
        const status = await zip.inflate(stream, zlib.CompressFlushMode.NO_FLUSH);
        if (status === zlib.ReturnStatus.STREAM_END) {
          break;
        }
        if (status !== zlib.ReturnStatus.OK) {
          throw new Error('gzip inflate failed: ' + status);
        }
        const availableIn = stream.availableIn === undefined ? -1 : stream.availableIn;
        const availableOut = stream.availableOut === undefined ? -1 : stream.availableOut;
        if (availableIn <= 0 && availableOut > 0) {
          throw new Error('gzip inflate stalled with ' + availableOut + ' bytes remaining');
        }
      }
      return new Uint8Array(outputBuffer);
    } finally {
      try {
        await zip.inflateEnd(stream);
      } catch (_) {
      }
    }
  }
}
