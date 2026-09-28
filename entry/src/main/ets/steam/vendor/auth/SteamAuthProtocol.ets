import { SteamBase64 } from '../protocol/SteamBase64';
import { SteamHex } from '../protocol/SteamHex';

/** Steam AuthenticationService endpoints used by the native auth flow. */
export enum SteamAuthEndpoint {
  GET_PASSWORD_RSA_PUBLIC_KEY = 'https://api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1/',
  BEGIN_AUTH_SESSION = 'https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaCredentials/v1/',
  UPDATE_STEAM_GUARD_CODE = 'https://api.steampowered.com/IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1/',
  POLL_AUTH_SESSION = 'https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1/',
  GENERATE_ACCESS_TOKEN = 'https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForAccount/v1/',
  // 2026-09-28 新增: QR 扫码登录 (宿主 SteamService 使用; 实测纯 HTTP 表单可用,
  // 上游"QR 需要 CM WebSocket"的判断不成立, 见 NOTICE.md)
  BEGIN_QR_SESSION = 'https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/'
}

export class SteamFormField {
  readonly name: string;
  readonly value: string;

  constructor(name: string, value: string) {
    this.name = name;
    this.value = value;
  }
}

export class SteamAuthHttpResponse {
  readonly statusCode: number;
  readonly body: string;

  constructor(statusCode: number, body: string) {
    this.statusCode = statusCode;
    this.body = body;
  }
}

/** Public RSA material returned by GetPasswordRSAPublicKey. */
export class SteamRsaPublicKey {
  readonly modulus: string;
  readonly exponent: string;
  readonly modulusBase64: string;
  readonly exponentBase64: string;
  readonly timestamp: number;

  constructor(modulus: string, exponent: string, timestamp: number) {
    this.modulus = modulus;
    this.exponent = exponent;
    // Kept as compatibility aliases for the original HTTP fixture-facing API.
    this.modulusBase64 = modulus;
    this.exponentBase64 = exponent;
    this.timestamp = timestamp;
  }

  /** Converts Steam modulus/exponent fields into an X.509 SubjectPublicKeyInfo PEM. */
  toPem(): string {
    const modulusBytes = SteamRsaPublicKey.integerBytes(SteamRsaPublicKey.decodeInteger(this.modulus));
    const exponentBytes = SteamRsaPublicKey.integerBytes(SteamRsaPublicKey.decodeInteger(this.exponent));
    const rsaKeyBody = SteamRsaPublicKey.sequence(
      SteamRsaPublicKey.integer(modulusBytes),
      SteamRsaPublicKey.integer(exponentBytes)
    );
    const algorithmIdentifier = new Uint8Array([
      0x30, 0x0d,
      0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01,
      0x05, 0x00
    ]);
    const bitString = SteamRsaPublicKey.sequenceBytes(
      new Uint8Array([0x03]),
      SteamRsaPublicKey.lengthBytes(rsaKeyBody.length + 1),
      new Uint8Array([0x00]),
      rsaKeyBody
    );
    const subjectPublicKeyInfo = SteamRsaPublicKey.sequenceBytes(
      new Uint8Array([0x30]),
      SteamRsaPublicKey.lengthBytes(algorithmIdentifier.length + bitString.length),
      algorithmIdentifier,
      bitString
    );
    return SteamRsaPublicKey.pem(subjectPublicKeyInfo);
  }

  private static decodeInteger(value: string): Uint8Array {
    const normalized = value.trim();
    if (SteamHex.isHex(normalized)) {
      return SteamHex.decode(normalized);
    }
    return SteamBase64.decode(normalized);
  }

  private static integerBytes(value: Uint8Array): Uint8Array {
    let start = 0;
    while (start + 1 < value.length && value[start] === 0) {
      start++;
    }
    if (value.length === 0) {
      return new Uint8Array([0]);
    }
    return value.slice(start);
  }

  private static integer(value: Uint8Array): Uint8Array {
    let prefixLength = 0;
    if (value.length > 0 && (value[0] & 0x80) !== 0) {
      prefixLength = 1;
    }
    const body = new Uint8Array(value.length + prefixLength);
    for (let index = 0; index < value.length; index++) {
      body[index + prefixLength] = value[index];
    }
    return SteamRsaPublicKey.sequenceBytes(
      new Uint8Array([0x02]),
      SteamRsaPublicKey.lengthBytes(body.length),
      body
    );
  }

  private static sequence(first: Uint8Array, second: Uint8Array): Uint8Array {
    return SteamRsaPublicKey.sequenceBytes(
      new Uint8Array([0x30]),
      SteamRsaPublicKey.lengthBytes(first.length + second.length),
      first,
      second
    );
  }

  private static sequenceBytes(...parts: Uint8Array[]): Uint8Array {
    let totalLength = 0;
    for (let index = 0; index < parts.length; index++) {
      totalLength += parts[index].length;
    }
    const result = new Uint8Array(totalLength);
    let offset = 0;
    for (let index = 0; index < parts.length; index++) {
      const part = parts[index];
      for (let partIndex = 0; partIndex < part.length; partIndex++) {
        result[offset + partIndex] = part[partIndex];
      }
      offset += part.length;
    }
    return result;
  }

  private static lengthBytes(length: number): Uint8Array {
    if (length < 128) {
      return new Uint8Array([length]);
    }
    const bytes: Array<number> = [];
    let remaining = length;
    while (remaining > 0) {
      bytes.unshift(remaining & 255);
      remaining = Math.floor(remaining / 256);
    }
    const result = new Uint8Array(bytes.length + 1);
    result[0] = 0x80 | bytes.length;
    for (let index = 0; index < bytes.length; index++) {
      result[index + 1] = bytes[index];
    }
    return result;
  }

  private static pem(value: Uint8Array): string {
    const encoded = SteamBase64.encode(value);
    let body = '';
    for (let index = 0; index < encoded.length; index += 64) {
      body += encoded.substring(index, Math.min(index + 64, encoded.length)) + '\n';
    }
    return '-----BEGIN PUBLIC KEY-----\n' + body + '-----END PUBLIC KEY-----\n';
  }
}
