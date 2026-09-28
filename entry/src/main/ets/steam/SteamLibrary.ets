import { http } from '@kit.NetworkKit';
import { hilog } from '../service/LogService';
import {
  SteamOwnedGamesWebApiEndpoint,
  SteamOwnedGamesWebApiParser
} from './vendor/library/SteamOwnedGamesWebApi';
import { SteamLibraryGame } from './vendor/models/SteamModels';
import { SteamAccountStore } from './SteamAccountStore';

const DOMAIN = 0x0000;
const TAG = 'SteamLibrary';
const CACHE_TTL_MS = 30 * 60 * 1000;

/** 持久缓存的可序列化条目（SteamLibraryGame 字段子集）。 */
interface SteamLibraryCacheEntry {
  appId: string;
  name: string;
  logoUrl: string;
  iconUrl: string;
  playtimeForeverSeconds: number;
  lastPlayedEpochSeconds: number;
}

interface SteamLibraryCacheBlob {
  savedAt: number;
  steamId64: string;
  games: Array<SteamLibraryCacheEntry>;
}

/**
 * Steam 游戏库（W4.1）：GetOwnedGames 拉取（access_token 免 Web API Key）
 * + 内存缓存 + steam 库缓存独立键持久化（30 分钟 TTL）。
 * URL/字段/CDN 拼接复用 vendor 的 Endpoint/Parser；HTTP 用宿主 GET。
 */
export class SteamLibraryService {
  private static instance: SteamLibraryService | null = null;

  static getInstance(): SteamLibraryService {
    if (!SteamLibraryService.instance) {
      SteamLibraryService.instance = new SteamLibraryService();
    }
    return SteamLibraryService.instance;
  }

  private store: SteamAccountStore = SteamAccountStore.getInstance();
  private cacheGames: Array<SteamLibraryGame> = [];
  private cacheSteamId: string = '';
  private cacheSavedAt: number = 0;
  private inFlight: Promise<Array<SteamLibraryGame>> | null = null;

  /** TTL 内的内存缓存是否命中。 */
  hasFreshCache(steamId64: string): boolean {
    return this.cacheSteamId === steamId64 && this.cacheGames.length > 0
      && Date.now() - this.cacheSavedAt < CACHE_TTL_MS;
  }

  cachedGames(): Array<SteamLibraryGame> {
    return this.cacheGames;
  }

  /** 拉取游戏库：内存缓存 → 持久缓存（TTL 内）→ 网络；网络失败回退过期持久缓存。 */
  async fetchOwnedGames(steamId64: string, accessToken: string,
    force: boolean = false): Promise<Array<SteamLibraryGame>> {
    if (steamId64.length === 0 || accessToken.length === 0) {
      throw new Error('尚未登录或会话未就绪，无法拉取游戏库');
    }
    if (!force) {
      if (this.hasFreshCache(steamId64)) {
        return this.cacheGames;
      }
      if (this.inFlight) {
        return this.inFlight;
      }
    }
    const task = this.loadGames(steamId64, accessToken, force);
    this.inFlight = task;
    try {
      return await task;
    } finally {
      if (this.inFlight === task) {
        this.inFlight = null;
      }
    }
  }

  private async loadGames(steamId64: string, accessToken: string,
    force: boolean): Promise<Array<SteamLibraryGame>> {
    if (!force) {
      const cached = await this.loadPersisted(steamId64);
      if (cached !== null) {
        this.cacheSteamId = steamId64;
        this.cacheSavedAt = Date.now();
        this.cacheGames = cached;
        return cached;
      }
    }
    const url = SteamOwnedGamesWebApiEndpoint.buildUrl(steamId64, accessToken);
    let games: Array<SteamLibraryGame>;
    try {
      const body = await this.httpGetJson(url);
      const result = SteamOwnedGamesWebApiParser.parse(body);
      games = result.games;
      hilog.info(DOMAIN, TAG, 'owned games fetched; games=%{public}d, total=%{public}d',
        games.length, result.totalCount);
      await this.persist(steamId64, games);
    } catch (e) {
      // 网络失败: 回退过期持久缓存（有总比没有强, 标注数据是旧的）
      const stale = await this.loadPersisted(steamId64);
      if (stale !== null && stale.length > 0) {
        hilog.warn(DOMAIN, TAG, 'owned games fetch failed, using stale cache: %{public}s',
          JSON.stringify(e));
        this.cacheSteamId = steamId64;
        this.cacheSavedAt = Date.now() - CACHE_TTL_MS;
        this.cacheGames = stale;
        return stale;
      }
      throw new Error('拉取游戏库失败: ' + JSON.stringify(e));
    }
    this.cacheSteamId = steamId64;
    this.cacheSavedAt = Date.now();
    this.cacheGames = games;
    return games;
  }

  /** 读持久缓存；TTL 内且 steamid 匹配才返回（JSON.parse 回填模型）。 */
  private async loadPersisted(steamId64: string): Promise<Array<SteamLibraryGame> | null> {
    try {
      const raw = await this.store.loadLibraryCache();
      if (!raw || raw.length === 0) return null;
      const blob = JSON.parse(raw) as SteamLibraryCacheBlob;
      if (!blob || typeof blob !== 'object') return null;
      if (blob.steamId64 !== steamId64) return null;
      if (!blob.games || !Array.isArray(blob.games)) return null;
      if (Date.now() - blob.savedAt >= CACHE_TTL_MS) return null;
      const games: Array<SteamLibraryGame> = [];
      for (const entry of blob.games) {
        if (!entry || typeof entry.appId !== 'string') continue;
        games.push(new SteamLibraryGame(
          entry.appId,
          typeof entry.name === 'string' ? entry.name : '',
          typeof entry.logoUrl === 'string' ? entry.logoUrl : '',
          typeof entry.iconUrl === 'string' ? entry.iconUrl : '',
          typeof entry.playtimeForeverSeconds === 'number' ? entry.playtimeForeverSeconds : 0,
          0,
          typeof entry.lastPlayedEpochSeconds === 'number' ? entry.lastPlayedEpochSeconds : 0,
          false
        ));
      }
      return games;
    } catch (_) {
      return null;
    }
  }

  private async persist(steamId64: string, games: Array<SteamLibraryGame>): Promise<void> {
    const entries: Array<SteamLibraryCacheEntry> = [];
    for (const game of games) {
      entries.push({
        appId: game.appId,
        name: game.name,
        logoUrl: game.logoUrl,
        iconUrl: game.iconUrl,
        playtimeForeverSeconds: game.playtimeForeverSeconds,
        lastPlayedEpochSeconds: game.lastPlayedEpochSeconds
      });
    }
    const blob: SteamLibraryCacheBlob = {
      savedAt: Date.now(),
      steamId64: steamId64,
      games: entries
    };
    try {
      await this.store.saveLibraryCache(JSON.stringify(blob));
    } catch (_) {
      // 持久化失败不致命（下次再拉）
    }
  }

  private async httpGetJson(url: string): Promise<string> {
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        header: {
          'Accept': 'application/json',
          'Referer': 'https://store.steampowered.com/'
        },
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode !== 200) {
        throw new Error('HTTP ' + response.responseCode);
      }
      return String(response.result);
    } finally {
      request.destroy();
    }
  }
}
