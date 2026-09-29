import { SteamProtoReader, SteamProtoWriter } from './vendor/protocol/SteamVarint';
import { SteamCmMessageType } from './vendor/protocol/SteamCmProtocol';

/**
 * Steam 内容协议 (W6) 的消息编解码 —— 字段号对照
 * SteamDatabase/Protobufs 的 steammessages_clientserver_2.steamclient 与
 * steammessages_contenthub.steamclient：
 * - CClientPICSProductInfoRequest / _Response  (EMsg 5202/5203)
 * - CContentServerDirectory_GetServersForSteamPipe_Request/_Response (ServiceMethod)
 * - CDepotManifest_GetDepotManifestRequest/_Response            (ServiceMethod)
 * - CDepotChunkDepot GetDepotDecryptionKeys                    (ServiceMethod)
 * - CContentServerDirectory_GetCDNAuthToken_Request/_Response  (ServiceMethod)
 */

// ===== PICS ProductInfo (EMsg 5202 请求 / 5203 响应) =====

/** CMsgClientPICSProductInfoRequest (EMsg 5202): apps=1{appid=1,access_token=2}, meta_data_only=2。 */
export class PicsAppInfoRequest {
  appIds: Array<number> = [];
  /** true 只取 meta; 拿 depot 列表必须 false。 */
  metaDataOnly: boolean = false;

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    for (const appId of this.appIds) {
      const inner = new SteamProtoWriter();
      inner.writeUInt32(1, appId);
      inner.writeUInt64(2, '0'); // access_token (public 信息 0 即可)
      writer.writeMessage(1, inner.toUint8Array());
    }
    if (this.metaDataOnly) {
      writer.writeBool(2, true);
    }
    return writer.toUint8Array();
  }
}

/** EMsg 常量 (steammessages_clientserver_2): PICS ProductInfo 请求/响应。 */
export const EMSG_PICS_PRODUCT_INFO_REQUEST = 5202;
export const EMSG_PICS_PRODUCT_INFO_RESPONSE = 5203;

export class PicsDepotInfo {
  depotId: number = 0;
  /** 全部 depot 分片大小合计 (bytes)。 */
  sizeOnDisk: number = 0;
  /** manifest GID (uint64 十进制字符串)。 */
  manifestGid: string = '';
  sharedDepot: boolean = false;
}

export class PicsAppInfo {
  appId: number = 0;
  /** depots 子消息里的 depot 条目。 */
  depots: Array<PicsDepotInfo> = [];
}

export class PicsProductInfoResponse {
  apps: Array<PicsAppInfo> = [];

  static decode(body: Uint8Array): PicsProductInfoResponse {
    const result = new PicsProductInfoResponse();
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        result.apps.push(PicsProductInfoResponse.decodeApp(reader.readLengthDelimited()));
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return result;
  }

  private static decodeApp(body: Uint8Array): PicsAppInfo {
    const app = new PicsAppInfo();
    app.appId = 0;
    app.depots = [];
    // appinfo 外层: status=1, appid=2, buffer=3 (序列化的 appinfo 子消息)
    const outer = new SteamProtoReader(body);
    let buffer = new Uint8Array(0);
    for (;;) {
      const tag = outer.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 2 && tag.wireType === 0) {
        app.appId = outer.readVarintNumber();
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) {
        buffer = outer.readLengthDelimited();
      } else {
        outer.skipField(tag.wireType);
      }
    }
    if (buffer.length === 0) {
      return app;
    }
    // 内层 buffer = Steam appinfo 消息: appid=0, depots=13
    const inner = new SteamProtoReader(buffer);
    for (;;) {
      const tag = inner.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 13 && tag.wireType === 2) {
        PicsProductInfoResponse.decodeDepots(inner.readLengthDelimited(), app);
      } else {
        inner.skipField(tag.wireType);
      }
    }
    return app;
  }

  private static decodeDepots(body: Uint8Array, app: PicsAppInfo): void {
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        const depot = PicsProductInfoResponse.decodeDepot(reader.readLengthDelimited());
        if (depot.depotId > 0) {
          app.depots.push(depot);
        }
      } else {
        reader.skipField(tag.wireType);
      }
    }
  }

  private static decodeDepot(body: Uint8Array): PicsDepotInfo {
    const depot = new PicsDepotInfo();
    depot.depotId = 0;
    depot.sizeOnDisk = 0;
    depot.manifestGid = '';
    depot.sharedDepot = false;
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        depot.depotId = reader.readVarintNumber();
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        // depotfromapp 子消息 → shared depot
        const shared = new SteamProtoReader(reader.readLengthDelimited());
        for (;;) {
          const sharedTag = shared.readTag();
          if (sharedTag === null) {
            break;
          }
          if (sharedTag.fieldNumber === 1 && sharedTag.wireType === 0) {
            depot.sharedDepot = true;
            depot.depotId = shared.readVarintNumber();
          } else {
            shared.skipField(sharedTag.wireType);
          }
        }
      } else if (tag.fieldNumber === 4 && tag.wireType === 2) {
        // manifests 子消息: field 1 = gid (string 形式的 uint64)
        const manifests = new SteamProtoReader(reader.readLengthDelimited());
        for (;;) {
          const mTag = manifests.readTag();
          if (mTag === null) {
            break;
          }
          if (mTag.fieldNumber === 1 && mTag.wireType === 2) {
            depot.manifestGid = SteamProtoReaderBytesHelper.readUtf8(manifests.readLengthDelimited());
          } else {
            manifests.skipField(mTag.wireType);
          }
        }
      } else if (tag.fieldNumber === 6 && tag.wireType === 2) {
        // sizeondisk 在 JSON 形态是 string, wire 上也是 length-delimited
        depot.sizeOnDisk = SteamProtoReaderBytesHelper.readUtf8Number(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return depot;
  }
}

/** length-delimited 字段的字符串/数字读取辅助 (vendor reader 无此便捷方法)。 */
class SteamProtoReaderBytesHelper {
  static readUtf8(bytes: Uint8Array): string {
    let text = '';
    let index = 0;
    while (index < bytes.length) {
      const code = bytes[index];
      if (code < 0x80) {
        text += String.fromCharCode(code);
        index += 1;
      } else if (code < 0xE0) {
        text += String.fromCharCode(((code & 0x1F) << 6) | (bytes[index + 1] & 0x3F));
        index += 2;
      } else {
        text += String.fromCharCode(((code & 0x0F) << 12)
          | ((bytes[index + 1] & 0x3F) << 6) | (bytes[index + 2] & 0x3F));
        index += 3;
      }
    }
    return text;
  }

  static readUtf8Number(bytes: Uint8Array): number {
    const text = SteamProtoReaderBytesHelper.readUtf8(bytes);
    const parsed = Number(text);
    return Number.isSafeInteger(parsed) ? parsed : 0;
  }
}

// ===== ContentServerDirectory.GetServersForSteamPipe =====

export class CdnServer {
  host: string = '';
  /** 'SteamCache' 类型走 CDN 分块下载。 */
  type: string = '';
  sourceId: number = 0;
}

export class GetServersForSteamPipeRequest {
  cellId = 0;
  /** 按 appid 请求数量无硬性要求; 一次请求即可。 */
  maxServers = 20;

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.cellId);
    writer.writeUInt32(2, 2); // field 2 = num_storage_apps? 保留 2
    return writer.toUint8Array();
  }
}

export class GetServersForSteamPipeResponse {
  servers: Array<CdnServer> = [];

  static decode(body: Uint8Array): GetServersForSteamPipeResponse {
    const result = new GetServersForSteamPipeResponse();
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        const server = GetServersForSteamPipeResponse.decodeServer(reader.readLengthDelimited());
        if (server.host.length > 0) {
          result.servers.push(server);
        }
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return result;
  }

  private static decodeServer(body: Uint8Array): CdnServer {
    const server = new CdnServer();
    server.host = '';
    server.type = '';
    server.sourceId = 0;
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        server.type = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        server.sourceId = reader.readVarintNumber();
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) {
        server.host = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return server;
  }
}

// ===== GetCDNAuthToken =====

export class GetCdnAuthTokenResponse {
  accessToken = '';
  /** 过期 epoch 秒。 */
  expirationTime = 0;

  static decode(body: Uint8Array): GetCdnAuthTokenResponse {
    const result = new GetCdnAuthTokenResponse();
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        result.accessToken = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        result.expirationTime = reader.readVarintNumber();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return result;
  }
}

export class GetCdnAuthTokenRequest {
  depotId = 0;
  appId = 0;

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.depotId);
    writer.writeUInt32(2, this.appId);
    return writer.toUint8Array();
  }
}

// ===== Depot Manifest 解析 (ProtoManifest 等价物) =====

export class ManifestFile {
  path: string = '';
  size: number = 0;
  /** flag 位 64 = 目录 (size 0 无 chunks)。 */
  flags: number = 0;
  chunks: Array<ManifestChunk> = [];
}

export class ManifestChunk {
  /** chunk 的 SHA-1 (十六进制), 同时是 CDN 请求路径段。 */
  sha: string = '';
  /** CDN 上的压缩后大小。 */
  compressedSize: number = 0;
  /** 解压后大小。 */
  uncompressedSize: number = 0;
}

export class DepotManifest {
  files: Array<ManifestFile> = [];

  static decode(body: Uint8Array): DepotManifest {
    const manifest = new DepotManifest();
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      // field 1 = mappings (repeated)
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        const file = DepotManifest.decodeFile(reader.readLengthDelimited());
        if (file.path.length > 0) {
          manifest.files.push(file);
        }
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return manifest;
  }

  private static decodeFile(body: Uint8Array): ManifestFile {
    const file = new ManifestFile();
    file.path = '';
    file.size = 0;
    file.flags = 0;
    file.chunks = [];
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        file.path = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        file.chunks.push(DepotManifest.decodeChunk(reader.readLengthDelimited()));
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        file.size = Number(reader.readVarintString());
      } else if (tag.fieldNumber === 4 && tag.wireType === 0) {
        file.flags = reader.readVarintNumber();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return file;
  }

  private static decodeChunk(body: Uint8Array): ManifestChunk {
    const chunk = new ManifestChunk();
    chunk.sha = '';
    chunk.compressedSize = 0;
    chunk.uncompressedSize = 0;
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 2) {
        chunk.sha = DepotManifest.toHex(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        chunk.uncompressedSize = Number(reader.readVarintString());
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        chunk.compressedSize = Number(reader.readVarintString());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return chunk;
  }

  private static toHex(bytes: Uint8Array): string {
    let hex = '';
    for (let index = 0; index < bytes.length; index++) {
      hex += (bytes[index] < 16 ? '0' : '') + bytes[index].toString(16);
    }
    return hex;
  }
}

/** GetDepotManifest 请求 (ServiceMethod: Client.DepotManifest.GetDepotManifest#1)。 */
export class GetDepotManifestRequest {
  depotId = 0;
  appId = 0;
  /** manifest GID 十进制字符串。 */
  manifestGid = '';

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.depotId);
    writer.writeUInt32(2, this.appId);
    writer.writeString(3, this.manifestGid);
    return writer.toUint8Array();
  }
}

/** ServiceMethodResponse 解包 (field 1 = jobname, field 2 = 序列化响应)。 */
export class ServiceMethodSupport {
  static readResponsePayload(body: Uint8Array): Uint8Array {
    const reader = new SteamProtoReader(body);
    let payload = new Uint8Array(0);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 2 && tag.wireType === 2) {
        payload = reader.readLengthDelimited();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return payload;
  }

  /** 判断一条 CM 消息是不是 ServiceMethodResponse (147)。 */
  static isServiceMethodResponse(messageType: number): boolean {
    return messageType === SteamCmMessageType.SERVICE_METHOD_RESPONSE;
  }
}
