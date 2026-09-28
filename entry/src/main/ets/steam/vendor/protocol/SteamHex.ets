/** Strict hexadecimal codec used by Steam AuthenticationService responses. */
export class SteamHex {
  static isHex(value: string): boolean {
    const normalized = value.trim();
    if (normalized.length === 0 || normalized.length % 2 !== 0) {
      return false;
    }
    for (let index = 0; index < normalized.length; index++) {
      if (SteamHex.digit(normalized.charCodeAt(index)) < 0) {
        return false;
      }
    }
    return true;
  }

  static decode(value: string): Uint8Array {
    const normalized = value.trim();
    if (!SteamHex.isHex(normalized)) {
      throw new Error('Invalid hexadecimal data');
    }
    const result = new Uint8Array(normalized.length / 2);
    for (let index = 0; index < result.length; index++) {
      const high = SteamHex.digit(normalized.charCodeAt(index * 2));
      const low = SteamHex.digit(normalized.charCodeAt(index * 2 + 1));
      result[index] = high * 16 + low;
    }
    return result;
  }

  private static digit(code: number): number {
    if (code >= 48 && code <= 57) {
      return code - 48;
    }
    if (code >= 65 && code <= 70) {
      return code - 55;
    }
    if (code >= 97 && code <= 102) {
      return code - 87;
    }
    return -1;
  }
}
