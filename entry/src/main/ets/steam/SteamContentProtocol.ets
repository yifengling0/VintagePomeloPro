import { SteamProtoReader, SteamProtoWriter } from './vendor/protocol/SteamVarint';
import { SteamPicsNode, SteamPicsVdf } from './SteamPicsVdf';
import { SteamBase64 } from './vendor/protocol/SteamBase64';
import { SteamCryptoAdapter } from './SteamCryptoAdapter';

/**
 * Fields follow SteamDatabase's appinfo/content directory/manifest protos.
 * PICS appinfo's buffer is a text KeyValues document.
 */

// ===== PICS ProductInfo (EMsg 8903/8904) =====

/** CMsgClientPICSProductInfoRequest: apps=2, meta_data_only=3. */
export class PicsAppInfoRequest {
  appIds: Array<number> = [];
  accessToken: string = '0';
  /** true 只取 meta; 拿 depot 列表必须 false。 */
  metaDataOnly: boolean = false;

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    for (const appId of this.appIds) {
      const inner = new SteamProtoWriter();
      inner.writeUInt32(1, appId);
      inner.writeUInt64(2, this.accessToken);
      writer.writeMessage(2, inner.toUint8Array());
    }
    if (this.metaDataOnly) {
      writer.writeBool(3, true);
    }
    writer.writeBool(7, true); // one response for this single-app request
    return writer.toUint8Array();
  }
}

/** EMsg values from enums_clientserver.proto. */
export const EMSG_PICS_PRODUCT_INFO_REQUEST = 8903;
export const EMSG_PICS_PRODUCT_INFO_RESPONSE = 8904;
export const EMSG_PICS_ACCESS_TOKEN_REQUEST = 8905;
export const EMSG_PICS_ACCESS_TOKEN_RESPONSE = 8906;
export const EMSG_GET_DEPOT_KEY_REQUEST = 5438;
export const EMSG_GET_DEPOT_KEY_RESPONSE = 5439;

export class PicsAppAccessTokenRequest {
  static encode(appId: number): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(2, appId);
    return writer.toUint8Array();
  }
}

export class PicsAppAccessTokenResponse {
  static decode(body: Uint8Array, appId: number): string {
    const reader = new SteamProtoReader(body);
    let denied = false;
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 3 && tag.wireType === 2) {
        const entry = new SteamProtoReader(reader.readLengthDelimited());
        let returnedAppId = 0;
        let accessToken = '0';
        for (;;) {
          const entryTag = entry.readTag();
          if (entryTag === null) break;
          if (entryTag.fieldNumber === 1 && entryTag.wireType === 0) returnedAppId = entry.readVarintNumber();
          else if (entryTag.fieldNumber === 2 && entryTag.wireType === 0) accessToken = entry.readVarintString();
          else entry.skipField(entryTag.wireType);
        }
        if (returnedAppId === appId && accessToken !== '0') return accessToken;
      } else if (tag.fieldNumber === 4 && tag.wireType === 0) {
        denied = reader.readVarintNumber() === appId || denied;
      } else {
        reader.skipField(tag.wireType);
      }
    }
    if (denied) throw new Error('Steam 未授权访问此游戏的内容资料');
    throw new Error('Steam 未返回此游戏的 PICS 访问令牌');
  }
}

export class GetDepotKeyRequest {
  static encode(appId: number, depotId: number): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, depotId);
    writer.writeUInt32(2, appId);
    return writer.toUint8Array();
  }
}

export class GetDepotKeyResponse {
  static decode(body: Uint8Array): Uint8Array {
    const reader = new SteamProtoReader(body);
    let result = 2;
    let key = new Uint8Array(0);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 1 && tag.wireType === 0) result = reader.readVarintNumber();
      else if (tag.fieldNumber === 3 && tag.wireType === 2) key = reader.readLengthDelimited();
      else reader.skipField(tag.wireType);
    }
    if (result !== 1 || key.length !== 32) throw new Error('Steam did not grant the depot decryption key');
    return key;
  }
}

export class PicsDepotInfo {
  depotId: number = 0;
  /** 全部 depot 分片大小合计 (bytes)。 */
  sizeOnDisk: number = 0;
  /** manifest GID (uint64 十进制字符串)。 */
  manifestGid: string = '';
  sharedDepot: boolean = false;
  /** depotfromapp: 共享 depot 的归属 app (redist / 引擎内容所在游戏)。 */
  depotFromApp: number = 0;
}

export class PicsAppInfo {
  appId: number = 0;
  /** depots 子消息里的 depot 条目。 */
  depots: Array<PicsDepotInfo> = [];
  bufferLength: number = 0;
  advertisedSize: number = 0;
  missingToken: boolean = false;
  onlyPublic: boolean = false;
  hasDepotsSection: boolean = false;
  shaHex: string = '';
}

export class PicsProductInfoResponse {
  apps: Array<PicsAppInfo> = [];
  httpHost: string = '';

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
      } else if (tag.fieldNumber === 8 && tag.wireType === 2) {
        result.httpHost = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
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
    // PICS AppInfo: appid=1, buffer=5 (NUL-terminated text KeyValues).
    const outer = new SteamProtoReader(body);
    let buffer = new Uint8Array(0);
    for (;;) {
      const tag = outer.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        app.appId = outer.readVarintNumber();
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        app.missingToken = outer.readBool();
      } else if (tag.fieldNumber === 4 && tag.wireType === 2) {
        const sha = outer.readLengthDelimited();
        if (sha.length === 20) {
          for (const byte of sha) app.shaHex += (byte < 16 ? '0' : '') + byte.toString(16);
        }
      } else if (tag.fieldNumber === 5 && tag.wireType === 2) {
        buffer = outer.readLengthDelimited();
      } else if (tag.fieldNumber === 6 && tag.wireType === 0) {
        app.onlyPublic = outer.readBool();
      } else if (tag.fieldNumber === 7 && tag.wireType === 0) {
        app.advertisedSize = outer.readVarintNumber();
      } else {
        outer.skipField(tag.wireType);
      }
    }
    app.bufferLength = buffer.length;
    if (buffer.length === 0) {
      return app;
    }
    // Steam returns a NUL-terminated text KeyValues document, not protobuf.
    PicsProductInfoResponse.parseAppBuffer(app, buffer);
    return app;
  }

  static parseAppBuffer(app: PicsAppInfo, buffer: Uint8Array): void {
    const root = SteamPicsVdf.parse(buffer);
    // Steam appinfo is commonly wrapped in an "appinfo" KeyValues object.
    const content = root.child('appinfo') ?? root.child(String(app.appId)) ?? root;
    const declaredAppId = content.text('appid');
    if (declaredAppId.length > 0 && declaredAppId !== String(app.appId)) {
      throw new Error('PICS appinfo ID 与请求的游戏不一致');
    }
    const depots = content.child('depots');
    app.hasDepotsSection = depots !== null;
    if (depots) PicsProductInfoResponse.decodeDepots(depots, app);
  }

  private static decodeDepots(node: SteamPicsNode, app: PicsAppInfo): void {
    node.children.forEach((entry: SteamPicsNode, key: string): void => {
      const depotId = Number(key);
      if (!Number.isSafeInteger(depotId) || depotId <= 0) return;
      const oslist = (entry.child('config')?.text('oslist') ?? '').toLowerCase();
      if (oslist.length > 0 && oslist.split(',').map((value: string): string => value.trim())
        .indexOf('windows') < 0) return;
      const publicManifest = entry.child('manifests')?.child('public');
      const gid = publicManifest?.text('gid') ?? publicManifest?.value ?? '';
      const shared = entry.text('depotfromapp').length > 0;
      if ((!/^\d+$/.test(gid) || gid === '0') && !shared) return;
      const depot = new PicsDepotInfo();
      depot.depotId = depotId;
      depot.manifestGid = /^\d+$/.test(gid) && gid !== '0' ? gid : '';
      depot.sharedDepot = shared;
      depot.depotFromApp = Number(entry.text('depotfromapp')) || 0;
      const size = Number(entry.text('maxsize'));
      depot.sizeOnDisk = Number.isSafeInteger(size) && size > 0 ? size : 0;
      app.depots.push(depot);
    });
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
      } else if ((code & 0xE0) === 0xC0 && index + 1 < bytes.length) {
        text += String.fromCharCode(((code & 0x1F) << 6) | (bytes[index + 1] & 0x3F));
        index += 2;
      } else if ((code & 0xF0) === 0xE0 && index + 2 < bytes.length) {
        text += String.fromCharCode(((code & 0x0F) << 12)
          | ((bytes[index + 1] & 0x3F) << 6) | (bytes[index + 2] & 0x3F));
        index += 3;
      } else if ((code & 0xF8) === 0xF0 && index + 3 < bytes.length) {
        const point = (((code & 7) << 18) | ((bytes[index + 1] & 63) << 12)
          | ((bytes[index + 2] & 63) << 6) | (bytes[index + 3] & 63)) - 0x10000;
        if (point < 0 || point > 0xFFFFF) throw new Error('Invalid Steam UTF-8 text');
        text += String.fromCharCode(0xD800 + (point >> 10), 0xDC00 + (point & 1023));
        index += 4;
      } else {
        throw new Error('Invalid Steam UTF-8 text');
      }
    }
    return text;
  }
}

// ===== ContentServerDirectory.GetServersForSteamPipe =====

export class CdnServer {
  host: string = '';
  vhost: string = '';
  /** 'SteamCache' 类型走 CDN 分块下载。 */
  type: string = '';
  httpsSupport: string = '';
  sourceId: number = 0;
}

export class GetServersForSteamPipeRequest {
  cellId = 0;
  /** 按 appid 请求数量无硬性要求; 一次请求即可。 */
  maxServers = 20;

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.cellId);
    writer.writeUInt32(2, this.maxServers);
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
      } else if (tag.fieldNumber === 8 && tag.wireType === 2) {
        server.host = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 9 && tag.wireType === 2) {
        server.vhost = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 12 && tag.wireType === 2) {
        server.httpsSupport = SteamProtoReaderBytesHelper.readUtf8(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    if (server.vhost.length > 0) server.host = server.vhost;
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
  hostName = '';

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.depotId);
    writer.writeString(2, this.hostName);
    writer.writeUInt32(3, this.appId);
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
  checksum: number = 0;
  offset: number = 0;
  /** CDN 上的压缩后大小。 */
  compressedSize: number = 0;
  /** 解压后大小。 */
  uncompressedSize: number = 0;
}

export class DepotManifest {
  files: Array<ManifestFile> = [];
  filenamesEncrypted: boolean = false;

  /** Steam manifest names are Base64(AES-ECB(IV) + AES-CBC(name)). */
  async decryptFilenames(key: Uint8Array): Promise<void> {
    if (!this.filenamesEncrypted) return;
    if (key.length !== 32) throw new Error('Invalid Steam depot key for filenames');
    const crypto = new SteamCryptoAdapter();
    const paths: Array<string> = [];
    for (const file of this.files) {
      const encoded = file.path.replace(/[ \t\r\n]/g, '');
      if (encoded.length === 0 || encoded.length > 8192 || encoded.length % 4 === 1 ||
        !/^[A-Za-z0-9+/_-]+={0,2}$/.test(encoded)) {
        throw new Error('Invalid encrypted Steam filename encoding');
      }
      const bytes = SteamBase64.decode(encoded);
      if (bytes.length < 32 || (bytes.length - 16) % 16 !== 0) {
        throw new Error('Invalid encrypted Steam filename length');
      }
      const iv = await crypto.aesEcbDecryptBlock(key, bytes.subarray(0, 16));
      const plaintext = await crypto.aesCbcDecrypt(key, iv, bytes.subarray(16));
      const length = plaintext.length > 0 && plaintext[plaintext.length - 1] === 0
        ? plaintext.length - 1 : plaintext.length;
      if (length === 0) throw new Error('Empty decrypted Steam filename');
      paths.push(SteamProtoReaderBytesHelper.readUtf8(plaintext.subarray(0, length)));
    }
    for (let index = 0; index < this.files.length; index++) this.files[index].path = paths[index];
    this.filenamesEncrypted = false;
  }

  static decode(body: Uint8Array): DepotManifest {
    const manifest = new DepotManifest();
    // CDN serves a ZIP with one file. Native code extracts that file before
    // this parser sees the payload/metadata/signature segments.
    if (body.length < 16 || DepotManifest.readUInt32(body, 0) !== 0x71F617D0) {
      throw new Error('Invalid Steam manifest payload magic');
    }
    const payloadLength = DepotManifest.readUInt32(body, 4);
    if (payloadLength <= 0 || payloadLength > body.length - 8) {
      throw new Error('Invalid Steam manifest payload length');
    }
    const next = 8 + payloadLength;
    if (next + 8 > body.length || DepotManifest.readUInt32(body, next) !== 0x1F4812BE) {
      throw new Error('Steam manifest metadata is missing');
    }
    const metadataLength = DepotManifest.readUInt32(body, next + 4);
    if (metadataLength > body.length - next - 8) throw new Error('Invalid Steam manifest metadata length');
    const metadata = new SteamProtoReader(body.subarray(next + 8, next + 8 + metadataLength));
    for (;;) {
      const tag = metadata.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 4 && tag.wireType === 0) {
        manifest.filenamesEncrypted = metadata.readBool();
      } else {
        metadata.skipField(tag.wireType);
      }
    }
    const reader = new SteamProtoReader(body.subarray(8, next));
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
      } else if (tag.fieldNumber === 6 && tag.wireType === 2) {
        file.chunks.push(DepotManifest.decodeChunk(reader.readLengthDelimited()));
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        file.size = Number(reader.readVarintString());
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
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
      } else if (tag.fieldNumber === 2 && tag.wireType === 5) {
        chunk.checksum = reader.readFixedUInt32();
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        chunk.offset = Number(reader.readVarintString());
      } else if (tag.fieldNumber === 4 && tag.wireType === 0) {
        chunk.uncompressedSize = Number(reader.readVarintString());
      } else if (tag.fieldNumber === 5 && tag.wireType === 0) {
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

  private static readUInt32(bytes: Uint8Array, offset: number): number {
    return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16)
      | (bytes[offset + 3] << 24)) >>> 0;
  }
}

/** CM returns a request code; the manifest itself is fetched from CDN. */
export class GetManifestRequestCodeRequest {
  depotId = 0;
  appId = 0;
  /** manifest GID 十进制字符串。 */
  manifestGid = '';

  encode(): Uint8Array {
    const writer = new SteamProtoWriter();
    writer.writeUInt32(1, this.appId);
    writer.writeUInt32(2, this.depotId);
    writer.writeUInt64(3, this.manifestGid);
    writer.writeString(4, 'public');
    return writer.toUint8Array();
  }
}

export class GetManifestRequestCodeResponse {
  static decode(body: Uint8Array): string {
    const reader = new SteamProtoReader(body);
    for (;;) {
      const tag = reader.readTag();
      if (tag === null) break;
      if (tag.fieldNumber === 1 && tag.wireType === 0) return reader.readVarintString();
      reader.skipField(tag.wireType);
    }
    throw new Error('Steam did not return a manifest request code');
  }
}
