import { hilog } from '../service/LogService';
import { http } from '@kit.NetworkKit';
import { SteamCmClient } from './vendor/cm/SteamCmClient';
import { SteamCmClientOptions } from './vendor/contracts/SteamCmContracts';
import { SteamCredentialSet } from './vendor/models/SteamModels';
import { SteamCmWebSocketTransport } from './SteamCmWebSocket';
import { SteamCmMultiDecompressor, SteamCmScheduler, SteamHostClock } from './SteamCmRuntime';
import { SteamChunkDecoder } from './SteamChunkDecoder';
import {
  DepotManifest,
  EMSG_PICS_PRODUCT_INFO_REQUEST,
  EMSG_PICS_PRODUCT_INFO_RESPONSE,
  GetCdnAuthTokenRequest,
  GetCdnAuthTokenResponse,
  GetDepotManifestRequest,
  GetServersForSteamPipeRequest,
  GetServersForSteamPipeResponse,
  CdnServer,
  PicsAppInfo,
  PicsAppInfoRequest,
  PicsDepotInfo,
  PicsProductInfoResponse,
  ServiceMethodSupport
} from './SteamContentProtocol';

const DOMAIN = 0x0000;
const TAG = 'SteamDownload';
const DEFAULT_CM_SERVER = 'cmp1-sea1.steamserver.net:443';
/** ServiceMethod 全名 (SteamKit2 命名空间)。 */
const METHOD_GET_SERVERS = 'ContentServerDirectory.GetServersForSteamPipe#1';
const METHOD_GET_CDN_AUTH = 'ContentServerDirectory.GetCDNAuthToken#1';
const METHOD_PICS_PRODUCT_INFO = 'Client.PICSProductInfo'; // 走 EMsg 5202, 非 ServiceMethod
/** 下载并发。 */
const DOWNLOAD_CONCURRENCY = 3;

/** 下载进度事件。 */
export interface SteamDownloadProgress {
  appId: number;
  phase: string;
  totalBytes: number;
  doneBytes: number;
  message: string;
}

export type SteamDownloadProgressListener = (progress: SteamDownloadProgress) => void;

/**
 * Steam 内容下载门面 (W6, 登录增强路径):
 * 已登录账号 → CM 会话 (refresh_token) → PICS 拿 depot 列表 →
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
  private progressListener: SteamDownloadProgressListener | null = null;
  private cdnServers: Array<CdnServer> = [];
  private cdnTokenCache: Map<string, GetCdnAuthTokenResponse> = new Map<string, GetCdnAuthTokenResponse>();

  onProgress(listener: SteamDownloadProgressListener): void {
    this.progressListener = listener;
  }

  private emit(appId: number, phase: string, totalBytes: number, doneBytes: number,
    message: string): void {
    if (this.progressListener) {
      this.progressListener({ appId, phase, totalBytes, doneBytes, message });
    }
  }

  /** 建立 CM 会话 (复用已登录账号的 refresh_token; 未登录抛错)。 */
  async ensureSession(accountName: string, refreshToken: string, steamId64: string): Promise<void> {
    if (this.cmClient && (this.cmClient.session.state === 'logged_on')) {
      return;
    }
    if (refreshToken.length === 0) {
      throw new Error('尚未登录 Steam, 无法建立下载会话');
    }
    if (this.cmClient) {
      try {
        await this.cmClient.disconnect();
      } catch (_) {
      }
      this.cmClient = null;
    }
    const wire = new SteamCmWebSocketTransport();
    const client = new SteamCmClient(
      accountName.length > 0 ? accountName : steamId64,
      new SteamCredentialSet(accountName, '', refreshToken),
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
    await client.connect();
    if (client.session.state !== 'logged_on' && client.session.state !== 'connected') {
      throw new Error('CM 会话未建立: ' + client.session.state);
    }
    this.cmClient = client;
    hilog.info(DOMAIN, TAG, 'CM session established; steamId=%{public}s', steamId64);
  }

  isSessionLive(): boolean {
    return this.cmClient !== null
      && (this.cmClient.session.state === 'logged_on' || this.cmClient.session.state === 'connected');
  }

  async closeSession(): Promise<void> {
    if (this.cmClient) {
      try {
        await this.cmClient.disconnect();
      } catch (_) {
      }
      this.cmClient = null;
    }
    this.cdnServers = [];
    this.cdnTokenCache.clear();
  }

  private requireClient(): SteamCmClient {
    if (!this.cmClient || !this.isSessionLive()) {
      throw new Error('CM 会话未建立');
    }
    return this.cmClient;
  }

  /** ServiceMethod 调用 (响应 body: field 1 = jobname, field 2 = 序列化结果)。 */
  private async callService(jobName: string, requestBytes: Uint8Array): Promise<Uint8Array> {
    const client = this.requireClient();
    const message = await client.call(jobName, requestBytes);
    return ServiceMethodSupport.readResponsePayload(message.body);
  }

  /** 拿一个 app 的 depot 列表: 正道是 PICS ProductInfo (EMsg 5202 经 CM 会话);
   * 免鉴权 appdetails 已不再返回 depots (Valve 收紧), 只做兜底。 */
  async fetchAppInfo(appId: number): Promise<PicsAppInfo> {
    const client = this.requireClient();
    const request = new PicsAppInfoRequest();
    request.appIds = [appId];
    const message = await client.callProto(EMSG_PICS_PRODUCT_INFO_REQUEST,
      EMSG_PICS_PRODUCT_INFO_RESPONSE, request.encode(), 'pics_product_info');
    const response = PicsProductInfoResponse.decode(message.body);
    if (response.apps.length === 0) {
      throw new Error('PICS 未返回该 app (未拥有或 appid 错误)');
    }
    const info = response.apps[0];
    const usable = info.depots.filter((depot: PicsDepotInfo): boolean =>
      depot.manifestGid.length > 0 && !depot.sharedDepot);
    if (usable.length === 0) {
      throw new Error('PICS 返回的 depot 无可用 manifest');
    }
    info.depots = usable;
    hilog.info(DOMAIN, TAG, 'PICS app=%{public}d depots=%{public}d', appId, usable.length);
    return info;
  }

  /** 免鉴权 appdetails 拿 depot 列表 (JSON)。仅公开信息; manifest 仍需 CM。 */
  private async fetchDepotsViaWebApi(appId: number): Promise<Array<PicsDepotInfo>> {
    const request = http.createHttp();
    try {
      const response = await request.request(
        `https://store.steampowered.com/api/appdetails?appids=${appId}`, {
        method: http.RequestMethod.GET,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode !== 200) {
        throw new Error('appdetails HTTP ' + response.responseCode);
      }
      // appdetails 返回形如 {"632360": {success, data:{depots:{"632360":{...}}}}};
      // ArkTS 无索引签名, JSON.parse 的动态对象用 Record 承载再 Object.keys 遍历
      const raw: Record<string, Object> = JSON.parse(String(response.result)) as Record<string, Object>;
      if (raw === null) {
        throw new Error('appdetails 返回为空');
      }
      const entryObj: Object | undefined = raw[appId.toString()];
      if (entryObj === undefined || entryObj === null) {
        throw new Error('appdetails 未返回该 app 的条目');
      }
      const entry = entryObj as AppDetailsEntry;
      if (!entry.success || !entry.data) {
        throw new Error('appdetails success=false (未拥有或区域限制)');
      }
      const depotsObj: Object | undefined = entry.data.depots;
      if (depotsObj === undefined || depotsObj === null) {
        throw new Error('appdetails 未返回 depots (该应用可能不可下载)');
      }
      const result = new Array<PicsDepotInfo>();
      const depotRecord = depotsObj as Record<string, Object>;
      const keys: Array<string> = Object.keys(depotRecord);
      for (const key of keys) {
        const depotValue: Object | undefined = depotRecord[key];
        const numeric = Number(key);
        if (!Number.isSafeInteger(numeric) || numeric <= 0 || depotValue === undefined
          || depotValue === null) {
          continue;
        }
        const depot = depotValue as DepotEntry;
        const item = new PicsDepotInfo();
        item.depotId = numeric;
        item.sizeOnDisk = typeof depot.maxsize === 'number' ? depot.maxsize : 0;
        item.manifestGid = depot.manifests !== undefined && depot.manifests !== null
          ? (depot.manifests['public'] ?? '') : '';
        item.sharedDepot = false;
        if (item.manifestGid.length > 0) {
          result.push(item);
        }
      }
      return result;
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
    const usable = response.servers.filter((server: CdnServer): boolean =>
      server.host.length > 0 && server.host.indexOf('steamcontent.com') >= 0);
    this.cdnServers = usable.length > 0 ? usable : response.servers;
    hilog.info(DOMAIN, TAG, 'CDN servers: %{public}d', this.cdnServers.length);
    if (this.cdnServers.length === 0) {
      throw new Error('未获取到 CDN 服务器');
    }
    return this.cdnServers;
  }

  /** depot 的 CDN 令牌 (缓存)。 */
  private async cdnToken(depotId: number, appId: number): Promise<string> {
    const key = String(depotId);
    const cached = this.cdnTokenCache.get(key);
    if (cached && cached.expirationTime * 1000 > Date.now() + 60000) {
      return cached.accessToken;
    }
    const request = new GetCdnAuthTokenRequest();
    request.depotId = depotId;
    request.appId = appId;
    const body = await this.callService(METHOD_GET_CDN_AUTH, request.encode());
    const response = GetCdnAuthTokenResponse.decode(body);
    if (response.accessToken.length === 0) {
      throw new Error('CDN 令牌为空 (depot ' + depotId + ')');
    }
    this.cdnTokenCache.set(key, response);
    return response.accessToken;
  }

  /** 拉取 depot manifest (当前 manifest GID)。 */
  async fetchManifest(appId: number, depotId: number, manifestGid: string): Promise<DepotManifest> {
    const request = new GetDepotManifestRequest();
    request.appId = appId;
    request.depotId = depotId;
    request.manifestGid = manifestGid;
    const body = await this.callService('Client.DepotManifest.GetDepotManifest#1', request.encode());
    const manifest = DepotManifest.decode(body);
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
    const token = await this.cdnToken(depotId, appId);
    const server = servers[0];
    const total = manifest.files.reduce((sum: number, file): number => sum + file.size, 0);
    let done = 0;
    for (const file of manifest.files) {
      // flag 64 = 目录
      if ((file.flags & 64) !== 0) {
        await fileSink.ensureDirectory(file.path);
        continue;
      }
      if (file.chunks.length === 0) {
        await fileSink.writeFile(file.path, new Uint8Array(0));
        continue;
      }
      const content = new Uint8Array(file.size);
      let offset = 0;
      for (const chunk of file.chunks) {
        const compressed = await this.downloadChunk(server.host, appId, depotId, chunk.sha, token);
        const bytes = SteamChunkDecoder.decode(compressed, chunk.uncompressedSize);
        content.set(bytes, offset);
        offset += chunk.uncompressedSize;
        done += chunk.uncompressedSize;
        this.emit(appId, 'downloading', total, done, file.path);
      }
      await fileSink.writeFile(file.path, content);
    }
  }

  private async downloadChunk(host: string, appId: number, depotId: number,
    chunkSha: string, token: string): Promise<Uint8Array> {
    const url = `https://${host}/depot/${depotId}/chunk/${chunkSha}?access_token=${encodeURIComponent(token)}`;
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 20000,
        readTimeout: 60000
      });
      if (response.responseCode !== 200) {
        throw new Error('chunk HTTP ' + response.responseCode + ' (' + chunkSha + ')');
      }
      return new Uint8Array(response.result as ArrayBuffer);
    } finally {
      request.destroy();
    }
  }
}

/** 下载落盘接口 (由调用方实现: 写 Download/games/<name>/...)。 */
export interface DepotFileSink {
  ensureDirectory(path: string): Promise<void>;
  writeFile(path: string, content: Uint8Array): Promise<void>;
}

interface DepotEntry {
  maxsize?: number;
  manifests?: Record<string, string>;
}

interface AppDetailsData {
  depots?: Object;
}

interface AppDetailsEntry {
  success?: boolean;
  data?: AppDetailsData;
}
