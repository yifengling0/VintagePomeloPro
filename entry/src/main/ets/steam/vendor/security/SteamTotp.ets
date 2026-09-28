import { IBinaryCrypto } from '../contracts/SteamPlatformContracts';

const STEAM_CODE_CHARACTERS: string = '23456789BCDFGHJKMNPQRTVWXY';
const STEAM_TOTP_WINDOW_SECONDS: number = 30;

/** Steam Guard TOTP calculation with HMAC supplied by the host platform. */
export class SteamTotp {
  /**
   * Generates the 5-character Steam Guard code for the given epoch second.
   * Steam aligns TOTP to 30-second windows: every raw epoch second inside the
   * same window must yield the same code, so the timestamp is floored to the
   * window before it is fed into the HMAC. Callers may pass any epoch second.
   */
  static async generateForTime(
    sharedSecretBase64: string,
    timeSeconds: number,
    crypto: IBinaryCrypto
  ): Promise<string> {
    if (timeSeconds < 0 || Math.floor(timeSeconds) !== timeSeconds) {
      throw new Error('Steam time must be a non-negative integer');
    }
    const secret = SteamTotp.decodeBase64(sharedSecretBase64);
    if (secret.length === 0) {
      throw new Error('Steam shared secret is empty');
    }
    const timeBytes = SteamTotp.encodeLittleEndianTime(
      Math.floor(timeSeconds / STEAM_TOTP_WINDOW_SECONDS)
    );
    const digest = await crypto.hmacSha1(secret, timeBytes);
    if (digest.length < 20) {
      throw new Error('HMAC-SHA1 digest must contain 20 bytes');
    }
    const offset = digest[19] & 15;
    if (offset + 3 >= digest.length) {
      throw new Error('HMAC-SHA1 dynamic offset is invalid');
    }
    const codeValue =
      (digest[offset] & 127) * 16777216 +
      digest[offset + 1] * 65536 +
      digest[offset + 2] * 256 +
      digest[offset + 3];
    let remaining = codeValue % 11881376;
    let code = '';
    for (let index = 0; index < 5; index++) {
      const characterIndex = remaining % STEAM_CODE_CHARACTERS.length;
      code = STEAM_CODE_CHARACTERS.charAt(characterIndex) + code;
      remaining = Math.floor(remaining / STEAM_CODE_CHARACTERS.length);
    }
    return code;
  }

  private static encodeLittleEndianTime(timeSeconds: number): Uint8Array {
    const bytes = new Uint8Array(8);
    let remaining = timeSeconds;
    for (let index = 0; index < bytes.length; index++) {
      bytes[index] = remaining % 256;
      remaining = Math.floor(remaining / 256);
    }
    return bytes;
  }

  private static decodeBase64(value: string): Uint8Array {
    const normalized = value.replace(/-/g, '+').replace(/_/g, '/');
    const bytes: Array<number> = [];
    let buffer = 0;
    let bits = 0;
    for (let index = 0; index < normalized.length; index++) {
      const character = normalized.charAt(index);
      if (character === '=') {
        break;
      }
      const digit = SteamTotp.base64Digit(character);
      if (digit < 0) {
        throw new Error('Steam shared secret is not valid Base64');
      }
      buffer = buffer * 64 + digit;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes.push(Math.floor(buffer / Math.pow(2, bits)) & 255);
        buffer = buffer % Math.pow(2, bits);
      }
    }
    return new Uint8Array(bytes);
  }

  private static base64Digit(character: string): number {
    const code = character.charCodeAt(0);
    if (code >= 65 && code <= 90) {
      return code - 65;
    }
    if (code >= 97 && code <= 122) {
      return code - 97 + 26;
    }
    if (code >= 48 && code <= 57) {
      return code - 48 + 52;
    }
    if (character === '+') {
      return 62;
    }
    if (character === '/') {
      return 63;
    }
    return -1;
  }
}