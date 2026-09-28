import { SteamJsonParser, SteamJsonValue, SteamJsonValueKind } from '../import/SteamJson';
import { SteamUtf8 } from '../protocol/SteamVarint';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';

/**
 * Static reader for the SteamID64 "sub" claim inside a Steam JWT refresh
 * token. Signature verification is intentionally out of scope: the token was
 * received from Steam over TLS, and the claim is only used to shortcut
 * identity lookup instead of opening a full CM logon.
 */
export class SteamJwt {
  /** Returns the trimmed subject claim, or an empty string when unreadable. */
  static subjectClaim(
    token: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): string {
    const trimmed: string = token.trim();
    const firstDot: number = trimmed.indexOf('.');
    if (firstDot <= 0) {
      return '';
    }
    const secondDot: number = trimmed.indexOf('.', firstDot + 1);
    if (secondDot <= firstDot + 1 || secondDot >= trimmed.length - 1) {
      return '';
    }
    const payloadJson: string = SteamJwt.decodePayload(trimmed.substring(firstDot + 1, secondDot));
    if (payloadJson.length === 0) {
      return '';
    }
    try {
      const payload: SteamJsonValue = SteamJsonParser.parse(payloadJson, logger);
      if (payload.kind !== SteamJsonValueKind.OBJECT) {
        return '';
      }
      const subject: SteamJsonValue | null = payload.getProperty('sub');
      if (subject === null || subject.kind !== SteamJsonValueKind.STRING) {
        return '';
      }
      return subject.stringValue.trim();
    } catch {
      return '';
    }
  }

  /** Base64url segment to UTF-8 JSON text; empty string when not decodable. */
  private static decodePayload(segment: string): string {
    const normalized: string = segment.replace(/-/g, '+').replace(/_/g, '/');
    const padding: number = (4 - normalized.length % 4) % 4;
    const padded: string = normalized + '===='.substring(0, padding);
    const bytes: Array<number> = [];
    let buffer: number = 0;
    let bits: number = 0;
    for (let index = 0; index < padded.length; index++) {
      const digit: number = SteamJwt.base64Digit(padded.charAt(index));
      if (digit < 0) {
        return '';
      }
      buffer = buffer * 64 + digit;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes.push(Math.floor(buffer / Math.pow(2, bits)) & 255);
        buffer = buffer % Math.pow(2, bits);
      }
    }
    return SteamUtf8.decode(new Uint8Array(bytes));
  }

  private static base64Digit(character: string): number {
    const code: number = character.charCodeAt(0);
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
