import { SteamProtoReader, SteamProtoWriter } from './SteamVarint';

/** Body of CMsgClientSessionToken from SteamKit2. */
export class SteamClientSessionToken {
  readonly token: string;

  constructor(token: string = '0') {
    this.token = token;
  }
}

/** Static protobuf codec for the transient CM session token. */
export class SteamClientSessionTokenCodec {
  static encode(value: SteamClientSessionToken): Uint8Array {
    const writer = new SteamProtoWriter();
    if (value.token !== '0') {
      writer.writeUInt64(1, value.token);
    }
    return writer.toUint8Array();
  }

  static decode(bytes: Uint8Array): SteamClientSessionToken {
    const reader = new SteamProtoReader(bytes);
    let token = '0';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        token = reader.readVarintString();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamClientSessionToken(token);
  }
}
