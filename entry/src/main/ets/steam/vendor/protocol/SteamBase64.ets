/** Standard and URL-safe Base64 helpers for Steam protocol payloads. */
export class SteamBase64 {
  private static readonly alphabet: string = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

  static encode(value: Uint8Array): string {
    let result = '';
    for (let index = 0; index < value.length; index += 3) {
      const first = value[index];
      const hasSecond = index + 1 < value.length;
      const hasThird = index + 2 < value.length;
      const second = hasSecond ? value[index + 1] : 0;
      const third = hasThird ? value[index + 2] : 0;
      result += SteamBase64.alphabet.charAt((first >> 2) & 63);
      result += SteamBase64.alphabet.charAt(((first & 3) << 4) | (second >> 4));
      result += hasSecond ? SteamBase64.alphabet.charAt(((second & 15) << 2) | (third >> 6)) : '=';
      result += hasThird ? SteamBase64.alphabet.charAt(third & 63) : '=';
    }
    return result;
  }

  static decode(value: string): Uint8Array {
    const normalized = value.replace(/-/g, '+').replace(/_/g, '/');
    const bytes: Array<number> = [];
    let buffer = 0;
    let bits = 0;
    for (let index = 0; index < normalized.length; index++) {
      const character = normalized.charAt(index);
      if (character === '=') {
        break;
      }
      const digit = SteamBase64.digit(character);
      if (digit < 0) {
        throw new Error('Invalid Base64 data');
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

  private static digit(character: string): number {
    return SteamBase64.alphabet.indexOf(character);
  }
}
