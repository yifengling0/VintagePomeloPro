import { SteamProtoReader, SteamProtoWriter } from './SteamVarint';

/** Body of the protobuf ClientHello message sent after a CM channel opens. */
export class SteamClientHelloRequest {
  readonly protocolVersion: number;

  constructor(protocolVersion: number = 65581) {
    if (protocolVersion <= 0 || protocolVersion > 4294967295 || Math.floor(protocolVersion) !== protocolVersion) {
      throw new Error('Steam CM protocol version is invalid');
    }
    this.protocolVersion = protocolVersion;
  }
}

/** Static codec for CMsgClientHello from SteamKit2. */
export class SteamClientHelloCodec {
  static encode(request: SteamClientHelloRequest): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, request.protocolVersion);
    return writer.toUint8Array();
  }

  static decode(bytes: Uint8Array): SteamClientHelloRequest {
    const reader = new SteamProtoReader(bytes);
    let protocolVersion = 0;
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        protocolVersion = reader.readVarintNumber();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamClientHelloRequest(protocolVersion);
  }
}
