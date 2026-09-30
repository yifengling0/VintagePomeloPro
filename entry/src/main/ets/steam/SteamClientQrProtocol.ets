import { SteamProtoReader, SteamProtoWriter, SteamUtf8 } from './vendor/protocol/SteamVarint';

/** CM Authentication.BeginAuthSessionViaQR / PollAuthSessionStatus payloads. */
export class SteamClientQrBeginResponse {
  clientId: string = '';
  requestId: Uint8Array = new Uint8Array(0);
  challengeUrl: string = '';
  intervalSeconds: number = 5;
}

export class SteamClientQrPollResponse {
  newClientId: string = '';
  newChallengeUrl: string = '';
  refreshToken: string = '';
  hadRemoteInteraction: boolean = false;
}

export class SteamClientQrProtocol {
  static beginRequest(): Uint8Array {
    const details = new SteamProtoWriter();
    details.writeString(1, 'VintagePomeloPro');
    details.writeInt32(2, 1); // EAuthTokenPlatformType.SteamClient
    details.writeInt32(3, 20); // EOSType.Win11, matching the Steam Client QR flow
    details.writeUInt32(4, 1); // desktop gaming device
    const request = new SteamProtoWriter();
    request.writeMessage(3, details.toUint8Array());
    return request.toUint8Array();
  }

  static decodeBegin(bytes: Uint8Array): SteamClientQrBeginResponse {
    const result = new SteamClientQrBeginResponse();
    const reader = new SteamProtoReader(bytes);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 1 && tag.wireType === 0) result.clientId = reader.readVarintString();
      else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        result.challengeUrl = SteamUtf8.decode(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) result.requestId = reader.readLengthDelimited();
      else if (tag.fieldNumber === 4 && tag.wireType === 5) result.intervalSeconds = reader.readFloat32();
      else reader.skipField(tag.wireType);
    }
    if (!result.clientId || !result.challengeUrl || result.requestId.length === 0) {
      throw new Error('Steam CM 未返回完整客户端授权二维码');
    }
    return result;
  }

  static pollRequest(clientId: string, requestId: Uint8Array): Uint8Array {
    const request = new SteamProtoWriter();
    request.writeUInt64(1, clientId);
    request.writeBytes(2, requestId);
    return request.toUint8Array();
  }

  static decodePoll(bytes: Uint8Array): SteamClientQrPollResponse {
    const result = new SteamClientQrPollResponse();
    const reader = new SteamProtoReader(bytes);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 1 && tag.wireType === 0) result.newClientId = reader.readVarintString();
      else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        result.newChallengeUrl = SteamUtf8.decode(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) {
        result.refreshToken = SteamUtf8.decode(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 5 && tag.wireType === 0) result.hadRemoteInteraction = reader.readBool();
      else reader.skipField(tag.wireType);
    }
    return result;
  }
}
