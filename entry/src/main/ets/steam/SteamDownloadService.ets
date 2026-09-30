import { hilog } from '../service/LogService';
import { http } from '@kit.NetworkKit';
import native from 'libsteamdecompress.so';
import { SteamCmClient } from './vendor/cm/SteamCmClient';
import { SteamCmClientOptions } from './vendor/contracts/SteamCmContracts';
import { SteamCmEvent, SteamCmEventListener, SteamCmEventType } from './vendor/contracts/SteamPlatformContracts';
import { SteamCredentialSet } from './vendor/models/SteamModels';
import { SteamCmWebSocketTransport } from './SteamCmWebSocket';
import { SteamCmMultiDecompressor, SteamCmScheduler, SteamHostClock } from './SteamCmRuntime';
import { SteamChunkDecoder } from './SteamChunkDecoder';
import { SteamCryptoAdapter } from './SteamCryptoAdapter';
import {
  DepotManifest,
  EMSG_PICS_ACCESS_TOKEN_REQUEST,
  EMSG_PICS_ACCESS_TOKEN_RESPONSE,
  EMSG_PICS_PRODUCT_INFO_REQUEST,
  EMSG_PICS_PRODUCT_INFO_RESPONSE,
  EMSG_GET_DEPOT_KEY_REQUEST,
  EMSG_GET_DEPOT_KEY_RESPONSE,
  GetDepotKeyRequest,
  GetDepotKeyResponse,
  GetCdnAuthTokenRequest,
  GetCdnAuthTokenResponse,
  GetManifestRequestCodeRequest,
  GetManifestRequestCodeResponse,
  GetServersForSteamPipeRequest,
  GetServersForSteamPipeResponse,
  CdnServer,
  PicsAppAccessTokenRequest,
  PicsAppAccessTokenResponse,
  PicsAppInfo,
  PicsAppInfoRequest,
  PicsDepotInfo,
  PicsProductInfoResponse
} from './SteamContentProtocol';

const DOMAIN = 0x0000;
const TAG = 'SteamDownload';
const DEFAULT_CM_SERVER = 'cmp1-sea1.steamserver.net:443';
/** ServiceMethod 全名 (SteamKit2 命名空间)。 */
const METHOD_GET_SERVERS = 'ContentServerDirectory.GetServersForSteamPipe#1';
const METHOD_GET_CDN_AUTH = 'ContentServerDirectory.GetCDNAuthToken#1';
const METHOD_MANIFEST_CODE = 'ContentServerDirectory.GetManifestRequestCode#1';

/** 下载进度事件。 */
export interface SteamDownloadProgress {
  appId: number;
  phase: string;
  totalBytes: number;
  doneBytes: number;
  message: string;
}

export type SteamDownloadProgressListener = (progress: SteamDownloadProgress) => void;

interface PublicCdnServer {
  host: string;
  vhost?: string;
  type: string;
  https_support?: string;
}

interface PublicCdnDirectoryResponse {
  servers: Array<PublicCdnServer>;
}

interface PublicCdnDirectory {
  response: PublicCdnDirectoryResponse;
}

/**
 * Steam 内容下载门面 (W6, 登录增强路径):
 * 已登录账号 → CM 会话 (Steam Client refresh_token) → PICS 拿 depot 列表 →
 * GetServersForSteamPipe / GetCDNAuthToken → 分块下载 → (解压在 native 层) 。
 * 红线: 只下载用户已拥有的内容 (license 校验由 Steam 服务端把守)。
 */
export class SteamDownloadService {
  private static instance: SteamDownloadService | null = null;

  static getInstance(): SteamDownloadService {
    if (!SteamDownloadService.instance) {
      SteamDownloadService.instance = new SteamDownloadService();
    }
    return SteamDownloadService.instance;
  }

  private cmClient: SteamCmClient | null = null;
  private sessionSteamId: string = '';
  private progressListener: SteamDownloadProgressListener | null = null;
  private cdnServers: Array<CdnServer> = [];
  private cdnTokenCache: Map<string, GetCdnAuthTokenResponse> = new Map<string, GetCdnAuthTokenResponse>();
  private depotKeys: Map<number, Uint8Array> = new Map<number, Uint8Array>();
  private activeAppId: number = 0;

  beginDownload(appId: number): void {
    if (this.activeAppId !== 0) throw new Error('已有 Steam 下载任务正在进行');
    this.activeAppId = appId;
  }

  endDownload(appId: number): void {
    if (this.activeAppId === appId) this.activeAppId = 0;
  }

  isDownloading(): boolean { return this.activeAppId !== 0; }

  onProgress(listener: SteamDownloadProgressListener): void {
    this.progressListener = listener;
  }

  removeProgressListener(listener: SteamDownloadProgressListener): void {
    if (this.progressListener === listener) this.progressListener = null;
  }

  private emit(appId: number, phase: string, totalBytes: number, doneBytes: number,
    message: string): void {
    if (this.progressListener) {
      this.progressListener({ appId, phase, totalBytes, doneBytes, message });
    }
  }

  /** 建立 CM 会话 (使用 Steam Client 平台 refresh_token; 未授权抛错)。 */
  async ensureSession(accountName: string, clientRefreshToken: string, steamId64: string): Promise<void> {
    if (this.cmClient && this.sessionSteamId === steamId64 &&
      this.cmClient.session.state === 'logged_on') {
      hilog.info(DOMAIN, TAG, 'ensureSession: cached live session');
      return;
    }
    if (clientRefreshToken.length === 0) {
      throw new Error('尚未授权 Steam 客户端下载');
    }
    if (this.cmClient) {
      hilog.info(DOMAIN, TAG, 'ensureSession: dropping old session (state=%{public}s)',
        String(this.cmClient.session.state));
      try {
        await this.cmClient.disconnect();
      } catch (_) {
      }
      this.cmClient = null;
    }
    this.sessionSteamId = '';
    this.cdnServers = [];
    this.cdnTokenCache.clear();
    this.depotKeys.clear();
    const wire = new SteamCmWebSocketTransport();
    const client = new SteamCmClient(
      accountName.length > 0 ? accountName : steamId64,
      new SteamCredentialSet(accountName, '', clientRefreshToken),
      wire,
      new SteamCmScheduler(),
      new SteamHostClock(),
      new SteamCmClientOptions(
        DEFAULT_CM_SERVER,
        steamId64,
        'VintagePomeloPro',
        65581,
        1771,
        'schinese',
        0,
        30,
        2,
        60,
        3,
        1,
        '',
        '',
        true,
        false,
        undefined,
        30000,
        new SteamCmMultiDecompressor()
      )
    );
    hilog.info(DOMAIN, TAG, 'ensureSession: connecting to %{public}s...', DEFAULT_CM_SERVER);
    try {
      await client.connect();
      hilog.info(DOMAIN, TAG, 'ensureSession: connect() returned, state=%{public}s',
        String(client.session.state));
      await this.waitForLogin(client);
    } catch (error) {
      await client.disconnect().catch((): void => {});
      throw error instanceof Error ? error : new Error('Steam CM connection failed');
    }
    this.cmClient = client;
    this.sessionSteamId = steamId64;
    hilog.info(DOMAIN, TAG, 'CM session established; steamId=%{public}s', steamId64);
  }

  private async waitForLogin(client: SteamCmClient): Promise<void> {
    if (client.session.state === 'logged_on') return;
    await new Promise<void>((resolve: () => void, reject: (error: Error) => void): void => {
      let timer = -1;
      const finish = (error: Error | null): void => {
        clearTimeout(timer);
        client.removeEventListener(listener);
        if (error) reject(error); else resolve();
      };
      const listener: SteamCmEventListener = (event: SteamCmEvent): void => {
        if (event.type === SteamCmEventType.ERROR || event.type === SteamCmEventType.AUTH_CHALLENGE ||
          event.type === SteamCmEventType.EXPIRED) {
          hilog.warn(DOMAIN, TAG, 'CM login event: type=%{public}s code=%{public}d message=%{public}s',
            String(event.type), event.errorCode, event.message);
        }
        if (event.type === SteamCmEventType.LOGGED_ON) finish(null);
        else if (event.type === SteamCmEventType.AUTH_CHALLENGE || event.type === SteamCmEventType.EXPIRED) {
          finish(new Error('Steam CM 登录凭据已失效，请重新登录'));
        } else if (event.type === SteamCmEventType.ERROR || event.type === SteamCmEventType.DISCONNECTED) {
          finish(new Error(event.errorCode === 5 || event.errorCode === 15
            ? 'Steam 客户端授权被拒，请重新扫码' : 'Steam CM 登录失败，请检查网络'));
        }
      };
      client.addEventListener(listener);
      timer = setTimeout((): void => finish(new Error('Steam CM 登录超时')), 30000);
      if (client.session.state === 'logged_on') finish(null);
    });
  }

  isSessionLive(): boolean {
    return this.cmClient !== null
      && this.cmClient.session.state === 'logged_on';
  }

  async closeSession(): Promise<void> {
    if (this.cmClient) {
      try {
        await this.cmClient.disconnect();
      } catch (_) {
      }
      this.cmClient = null;
    }
    this.sessionSteamId = '';
    this.cdnServers = [];
    this.cdnTokenCache.clear();
    this.depotKeys.clear();
  }

  private requireClient(): SteamCmClient {
    if (!this.cmClient || !this.isSessionLive()) {
      throw new Error('CM 会话未建立');
    }
    return this.cmClient;
  }

  /** The CM unified response body is the method's protobuf response. */
  private async callService(jobName: string, requestBytes: Uint8Array): Promise<Uint8Array> {
    const client = this.requireClient();
    const message = await client.call(jobName, requestBytes);
    return message.body;
  }

  /** Query depot manifests through PICS (EMsg 8903/8904). */
  async fetchAppInfo(appId: number): Promise<PicsAppInfo> {
    const client = this.requireClient();
    const request = new PicsAppInfoRequest();
    request.appIds = [appId];
    let message = await client.callProto(EMSG_PICS_PRODUCT_INFO_REQUEST,
      EMSG_PICS_PRODUCT_INFO_RESPONSE, request.encode(), 'pics_product_info');
    let response = PicsProductInfoResponse.decode(message.body);
    if (response.apps.length === 0) {
      throw new Error('PICS 未返回该 app (未拥有或 appid 错误)');
    }
    let info = response.apps[0];
    if (info.appId !== appId) throw new Error('PICS returned a different Steam app');
    if (info.missingToken || info.onlyPublic) {
      const tokenMessage = await client.callProto(EMSG_PICS_ACCESS_TOKEN_REQUEST,
        EMSG_PICS_ACCESS_TOKEN_RESPONSE, PicsAppAccessTokenRequest.encode(appId), 'pics_access_token');
      request.accessToken = PicsAppAccessTokenResponse.decode(tokenMessage.body, appId);
      message = await client.callProto(EMSG_PICS_PRODUCT_INFO_REQUEST,
        EMSG_PICS_PRODUCT_INFO_RESPONSE, request.encode(), 'pics_product_info_with_token');
      response = PicsProductInfoResponse.decode(message.body);
      if (response.apps.length === 0 || response.apps[0].appId !== appId) {
        throw new Error('PICS 访问令牌未返回该游戏资料');
      }
      info = response.apps[0];
    }
    if (info.bufferLength === 0 && info.advertisedSize > 0 && info.shaHex.length === 40) {
      const appBuffer = await this.fetchAppInfoBuffer(response.httpHost, info);
      PicsProductInfoResponse.parseAppBuffer(info, appBuffer);
      info.bufferLength = appBuffer.length;
    }
    hilog.info(DOMAIN, TAG,
      'PICS app=%{public}d bytes=%{public}d size=%{public}d depotsSection=%{public}s depots=%{public}d public=%{public}s missingToken=%{public}s',
      appId, info.bufferLength, info.advertisedSize, String(info.hasDepotsSection), info.depots.length,
      String(info.onlyPublic), String(info.missingToken));
    const usable = info.depots.filter((depot: PicsDepotInfo): boolean =>
      depot.manifestGid.length > 0 || depot.sharedDepot);
    if (usable.length === 0) {
      throw new Error('PICS 返回的 depot 无可用 manifest');
    }
    info.depots = usable;
    hilog.info(DOMAIN, TAG, 'PICS app=%{public}d depots=%{public}d', appId, usable.length);
    return info;
  }

  private async fetchAppInfoBuffer(host: string, info: PicsAppInfo): Promise<Uint8Array> {
    if (!/^[a-z0-9.-]+$/i.test(host) || !/(^|\.)(steamcontent\.com|steampowered\.com|steamstatic\.com|akamaized\.net)$/i.test(host)) {
      throw new Error('PICS appinfo HTTP host 无效');
    }
    if (info.advertisedSize > 16 * 1024 * 1024) throw new Error('PICS appinfo 太大');
    const url = `https://${host}/appinfo/${info.appId}/sha/${info.shaHex}.txt.gz`;
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        maxLimit: 16 * 1024 * 1024,
        connectTimeout: 20000,
        readTimeout: 60000
      }).catch(() => { throw new Error('PICS appinfo HTTP 请求失败'); });
      if (response.responseCode !== 200) throw new Error('PICS appinfo HTTP ' + response.responseCode);
      const bytes = new Uint8Array(response.result as ArrayBuffer);
      if (bytes.length === 0 || bytes.length > 16 * 1024 * 1024) throw new Error('PICS appinfo HTTP 内容无效');
      if (bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b) {
        return new SteamCmMultiDecompressor().decompress(bytes, info.advertisedSize);
      }
      return bytes;
    } finally {
      request.destroy();
    }
  }

  /** CDN 服务器列表 (GetServersForSteamPipe)。 */
  async fetchCdnServers(cellId: number = 0): Promise<Array<CdnServer>> {
    if (this.cdnServers.length > 0) {
      return this.cdnServers;
    }
    const request = new GetServersForSteamPipeRequest();
    request.cellId = cellId;
    const body = await this.callService(METHOD_GET_SERVERS, request.encode());
    const response = GetServersForSteamPipeResponse.decode(body);
    hilog.info(DOMAIN, TAG, 'CDN directory response bytes=%{public}d entries=%{public}d firstHost=%{public}s firstType=%{public}s firstHttps=%{public}s',
      body.length, response.servers.length, response.servers.length > 0 ? response.servers[0].host : '',
      response.servers.length > 0 ? response.servers[0].type : '',
      response.servers.length > 0 ? response.servers[0].httpsSupport : '');
    this.cdnServers = response.servers.filter((server: CdnServer): boolean =>
      (server.type === 'SteamCache' || server.type === 'CDN') && this.isApprovedCdnHost(server.host));
    if (this.cdnServers.length === 0) {
      this.cdnServers = await this.fetchPublicCdnServers(cellId);
    }
    hilog.info(DOMAIN, TAG, 'CDN servers: %{public}d', this.cdnServers.length);
    if (this.cdnServers.length === 0) {
      throw new Error('未获取到 CDN 服务器');
    }
    return this.cdnServers;
  }

  private async fetchPublicCdnServers(cellId: number): Promise<Array<CdnServer>> {
    const url = 'https://api.steampowered.com/IContentServerDirectoryService/' +
      'GetServersForSteamPipe/v1/?cell_id=' + cellId + '&max_servers=20';
    const request = http.createHttp();
    try {
      const result = await request.request(url, {
        method: http.RequestMethod.GET,
        maxLimit: 2 * 1024 * 1024,
        connectTimeout: 20000,
        readTimeout: 20000
      });
      if (result.responseCode !== 200 || typeof result.result !== 'string') return [];
      const parsed = JSON.parse(result.result as string) as PublicCdnDirectory;
      if (!parsed.response || !Array.isArray(parsed.response.servers)) return [];
      const servers: Array<CdnServer> = [];
      for (const entry of parsed.response.servers) {
        const host = entry.vhost || entry.host;
        if (entry.type !== 'SteamCache' || !this.isApprovedCdnHost(host)) continue;
        const server = new CdnServer();
        server.host = host;
        server.type = entry.type;
        server.httpsSupport = entry.https_support || 'mandatory';
        servers.push(server);
      }
      hilog.info(DOMAIN, TAG, 'Public CDN directory fallback entries=%{public}d', servers.length);
      return servers;
    } catch (_) {
      return [];
    } finally {
      request.destroy();
    }
  }

  private isApprovedCdnHost(host: string): boolean {
    return /^[a-z0-9.-]+$/i.test(host) && /(^|\.)(steamcontent\.com|clngaa\.com)$/i.test(host);
  }

  /** depot 的 CDN 令牌 (缓存)。 */
  private async cdnToken(depotId: number, appId: number, host: string): Promise<string> {
    const key = String(depotId) + '@' + host;
    const cached = this.cdnTokenCache.get(key);
    if (cached && cached.expirationTime * 1000 > Date.now() + 60000) {
      return cached.accessToken;
    }
    const request = new GetCdnAuthTokenRequest();
    request.depotId = depotId;
    request.appId = appId;
    request.hostName = host;
    const body = await this.callService(METHOD_GET_CDN_AUTH, request.encode());
    const response = GetCdnAuthTokenResponse.decode(body);
    this.cdnTokenCache.set(key, response);
    return response.accessToken;
  }

  private async depotKey(appId: number, depotId: number): Promise<Uint8Array> {
    const cached = this.depotKeys.get(depotId);
    if (cached) return cached;
    const message = await this.requireClient().callProto(EMSG_GET_DEPOT_KEY_REQUEST,
      EMSG_GET_DEPOT_KEY_RESPONSE, GetDepotKeyRequest.encode(appId, depotId), 'depot_key');
    const key = GetDepotKeyResponse.decode(message.body);
    this.depotKeys.set(depotId, key);
    return key;
  }

  /** 拉取 depot manifest (当前 manifest GID)。 */
  async fetchManifest(appId: number, depotId: number, manifestGid: string): Promise<DepotManifest> {
    const request = new GetManifestRequestCodeRequest();
    request.appId = appId;
    request.depotId = depotId;
    request.manifestGid = manifestGid;
    const codeBody = await this.callService(METHOD_MANIFEST_CODE, request.encode());
    const code = GetManifestRequestCodeResponse.decode(codeBody);
    const server = (await this.fetchCdnServers())[0];
    const token = await this.cdnToken(depotId, appId, server.host);
    const zipped = await this.fetchCdnBytes(server,
      `depot/${depotId}/manifest/${manifestGid}/5/${code}`, token, 64 * 1024 * 1024);
    const body = new Uint8Array(native.unzipManifest(zipped) as ArrayBuffer);
    const manifest = DepotManifest.decode(body);
    if (manifest.filenamesEncrypted) {
      await manifest.decryptFilenames(await this.depotKey(appId, depotId));
    }
    hilog.info(DOMAIN, TAG, 'manifest depot=%{public}d files=%{public}d', depotId, manifest.files.length);
    if (manifest.files.length === 0) {
      throw new Error('manifest 解析为空 (depot ' + depotId + ')');
    }
    return manifest;
  }

  /** 下载单个 depot 的全部 chunk, 经 fileSink 落盘。 */
  async downloadDepot(appId: number, depotId: number, manifestGid: string,
    fileSink: DepotFileSink): Promise<void> {
    const manifest = await this.fetchManifest(appId, depotId, manifestGid);
    const servers = await this.fetchCdnServers();
    const server = servers[0];
    const token = await this.cdnToken(depotId, appId, server.host);
    const key = await this.depotKey(appId, depotId);
    const total = manifest.files.reduce((sum: number, file): number => sum + file.size, 0);
    let done = 0;
    for (const file of manifest.files) {
      // flag 64 = 目录
      if ((file.flags & 64) !== 0) {
        await fileSink.ensureDirectory(file.path);
        continue;
      }
      if ((file.flags & 2) !== 0) throw new Error('Steam manifest contains an unsupported link');
      if (!Number.isSafeInteger(file.size) || file.size < 0) throw new Error('Invalid Steam file size');
      const ordered = file.chunks.slice().sort((left, right): number => left.offset - right.offset);
      let covered = 0;
      for (const chunk of ordered) {
        if (!Number.isSafeInteger(chunk.offset) || chunk.offset !== covered ||
          !Number.isSafeInteger(chunk.uncompressedSize) || chunk.uncompressedSize <= 0) {
          throw new Error('Steam manifest file has gaps or overlapping chunks');
        }
        covered += chunk.uncompressedSize;
        if (!Number.isSafeInteger(covered) || covered > file.size) {
          throw new Error('Steam manifest chunk exceeds file size');
        }
      }
      if (covered !== file.size) throw new Error('Steam manifest file is incomplete');
      await fileSink.prepareFile(file.path, file.size);
      if (file.chunks.length === 0) {
        if (file.size !== 0) throw new Error('Steam manifest file has no chunks');
        await fileSink.finishFile(file.path, file.size);
        continue;
      }
      let written = 0;
      for (const chunk of file.chunks) {
        const compressed = await this.downloadChunk(server, appId, depotId, chunk.sha, token);
        if (chunk.compressedSize > 0 && compressed.length !== chunk.compressedSize) {
          throw new Error('Steam chunk compressed size mismatch');
        }
        const decrypted = await this.decryptChunk(compressed, key);
        const bytes = SteamChunkDecoder.decode(decrypted, chunk.uncompressedSize);
        this.verifyChunk(bytes, chunk.checksum);
        if (!Number.isSafeInteger(chunk.offset) || chunk.offset < 0 ||
          chunk.offset + bytes.length > file.size) throw new Error('Steam chunk exceeds file size');
        await fileSink.writeChunk(file.path, chunk.offset, bytes);
        written += bytes.length;
        done += chunk.uncompressedSize;
        this.emit(appId, 'downloading', total, done, file.path);
      }
      if (written !== file.size) throw new Error('Steam file is incomplete: ' + file.path);
      await fileSink.finishFile(file.path, file.size);
    }
  }

  private async downloadChunk(server: CdnServer, appId: number, depotId: number,
    chunkSha: string, token: string): Promise<Uint8Array> {
    if (!/^[0-9a-f]{40}$/.test(chunkSha)) throw new Error('Invalid Steam chunk identifier');
    return this.fetchCdnBytes(server, `depot/${depotId}/chunk/${chunkSha}`, token, 128 * 1024 * 1024);
  }

  private async fetchCdnBytes(server: CdnServer, path: string, token: string,
    maximum: number): Promise<Uint8Array> {
    const host = server.host;
    if (!this.isApprovedCdnHost(host)) throw new Error('Invalid Steam CDN host');
    const query = token.replace(/^\?/, '');
    if (query.length > 2048 || (query.length > 0 && !/^[A-Za-z0-9_%=&.+~-]+$/.test(query))) {
      throw new Error('Invalid Steam CDN token');
    }
    const scheme = server.httpsSupport === 'mandatory' ? 'https' : 'http';
    const url = `${scheme}://${host}/${path}${query.length > 0 ? '?' + query : ''}`;
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        maxLimit: maximum,
        connectTimeout: 20000,
        readTimeout: 60000
      }).catch(() => { throw new Error('Steam CDN network request failed'); });
      if (response.responseCode !== 200) {
        throw new Error('Steam CDN HTTP ' + response.responseCode);
      }
      const bytes = new Uint8Array(response.result as ArrayBuffer);
      if (bytes.length === 0 || bytes.length > maximum) throw new Error('Steam CDN response size is invalid');
      return bytes;
    } finally {
      request.destroy();
    }
  }

  private async decryptChunk(bytes: Uint8Array, key: Uint8Array): Promise<Uint8Array> {
    if (bytes.length < 32 || (bytes.length - 16) % 16 !== 0) {
      throw new Error('Encrypted Steam chunk has invalid length');
    }
    const crypto = new SteamCryptoAdapter();
    const iv = await crypto.aesEcbDecryptBlock(key, bytes.subarray(0, 16));
    return crypto.aesCbcDecrypt(key, iv, bytes.subarray(16));
  }

  private verifyChunk(bytes: Uint8Array, expectedChecksum: number): void {
    // Steam's manifest CRC field is Adler-32 with seed 0 (SteamKit DepotChunk).
    let a = 0;
    let b = 0;
    for (let index = 0; index < bytes.length; index++) {
      a += bytes[index];
      b += a;
      if ((index & 4095) === 4095) { a %= 65521; b %= 65521; }
    }
    const actual = (((b % 65521) << 16) | (a % 65521)) >>> 0;
    if (actual !== expectedChecksum) throw new Error('Steam chunk checksum mismatch');
  }
}

/** Chunk sink for a staged Steam install. */
export interface DepotFileSink {
  ensureDirectory(path: string): Promise<void>;
  prepareFile(path: string, size: number): Promise<void>;
  writeChunk(path: string, offset: number, content: Uint8Array): Promise<void>;
  finishFile(path: string, size: number): Promise<void>;
}
