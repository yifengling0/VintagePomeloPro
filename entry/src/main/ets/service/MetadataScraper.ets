import { http } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from './LogService';
import { uniqueExactGameId } from '../model/GameArtworkRules';

const DOMAIN = 0x0000;
const TAG = 'MetadataScraper';
const SGDB_BASE = 'https://www.steamgriddb.com/api/v2';

/** 搜索候选 (来自 SteamGridDB autocomplete)。 */
export interface ScrapedGame {
  id: number;
  name: string;
  releaseYear: string;
}

/** 抓取结果: 供 UI 提示用。 */
export interface ScrapeResult {
  ok: boolean;
  /** 成功: 写盘后的封面绝对路径; 失败: 空串。 */
  coverPath: string;
  /** 命中的游戏名 (成功时) 或失败原因。 */
  detail: string;
}

interface SgdbListResponse {
  success: boolean;
  data: ScrapedGame[];
}

interface SgdbGridItem {
  id: number;
  url: string;
  thumb: string;
  width: number;
  height: number;
}

interface SgdbGridResponse {
  success: boolean;
  data: SgdbGridItem[];
}
interface SteamSearchItem { id: number; name: string; }
interface SteamSearchResponse { items: SteamSearchItem[]; }
interface JsonHeaders { Authorization?: string; Accept: string; }

/**
 * 封面元数据抓取 (SteamGridDB)。
 *
 * 设计要点:
 * - **展示侧零改动**: 抓到的图直接写进游戏目录命名 cover.png, 现有扫描链
 *   (AppCatalogRules.chooseCoverFileName 的优先级列表) 会自动发现;
 * - API Key 由用户在系统设置里填写 (免费申请: steamgriddb.com → Preferences
 *   → API), 存在独立 preferences 键里, 不进 GlobalSettings;
 * - 仅在用户主动点击"抓取封面"时联网 (不做后台批量抓取, 避免流量与封禁风险)。
 */
export class MetadataScraper {
  private static instance: MetadataScraper | null = null;

  static getInstance(): MetadataScraper {
    if (!MetadataScraper.instance) {
      MetadataScraper.instance = new MetadataScraper();
    }
    return MetadataScraper.instance;
  }

  /** 按名称搜索候选游戏。 */
  async search(apiKey: string, term: string): Promise<ScrapedGame[]> {
    const url = `${SGDB_BASE}/search/autocomplete/${encodeURIComponent(term.trim())}`;
    const json = await this.getJson(url, apiKey);
    if (!json) return [];
    try {
      const parsed = JSON.parse(json) as SgdbListResponse;
      if (!parsed || parsed.success !== true || !parsed.data) return [];
      return parsed.data;
    } catch (_) {
      return [];
    }
  }

  /** 取某游戏的竖版封面 (优先 600x900, SteamGridDB 的 grid 即竖版盒装图)。 */
  async fetchCoverUrl(apiKey: string, gameId: number): Promise<string> {
    const url = `${SGDB_BASE}/grids/game/${gameId}?dimensions=600x900,342x482`;
    const json = await this.getJson(url, apiKey);
    if (!json) return '';
    try {
      const parsed = JSON.parse(json) as SgdbGridResponse;
      if (!parsed || parsed.success !== true || !parsed.data || parsed.data.length === 0) return '';
      // 取面积最大的一张 (清晰度优先)。
      let best = parsed.data[0];
      for (let i = 1; i < parsed.data.length; i++) {
        const cur = parsed.data[i];
        if (cur.width * cur.height > best.width * best.height) best = cur;
      }
      return best.url;
    } catch (_) {
      return '';
    }
  }

  /**
   * 一站抓取: 按标题搜索 → 唯一准确匹配 → 下载竖版封面写入游戏目录。
   * Steam 公开图源无需 Key；仅在公开图源无图时尝试可选的 SteamGridDB。
   */
  async scrapeCover(apiKey: string, title: string, gameDirectory: string): Promise<ScrapeResult> {
    if (title.trim().length === 0 || gameDirectory.length === 0) {
      return { ok: false, coverPath: '', detail: '缺少游戏名称或目录' };
    }
    try {
      const searchUrl = 'https://store.steampowered.com/api/storesearch/?term=' +
        encodeURIComponent(title.trim()) + '&l=schinese&cc=CN';
      const steam = JSON.parse(await this.getJson(searchUrl, '')) as SteamSearchResponse;
      const steamId = uniqueExactGameId(steam.items ?? [], title);
      if (steamId > 0) {
        const publicUrl = 'https://cdn.cloudflare.steamstatic.com/steam/apps/' +
          steamId.toString() + '/library_600x900.jpg';
        const publicPath = await this.download(publicUrl, gameDirectory);
        if (publicPath) return { ok: true, coverPath: publicPath, detail: title };
      }
    } catch (_) {
      // A public lookup failure still permits the optional supplementary source.
    }
    if (!apiKey.trim()) {
      return { ok: false, coverPath: '',
        detail: '公开图源未找到唯一匹配的竖版封面，可手动选图或在外观设置填写 SteamGridDB Key' };
    }
    const candidates = await this.search(apiKey, title);
    if (candidates.length === 0) {
      return { ok: false, coverPath: '', detail: '未找到匹配的游戏' };
    }
    const gameId = uniqueExactGameId(candidates, title);
    if (gameId <= 0) {
      return { ok: false, coverPath: '', detail: '未找到唯一准确匹配的游戏，请手动选择封面' };
    }
    const matched = candidates.find((candidate: ScrapedGame): boolean => candidate.id === gameId)!;
    const coverUrl = await this.fetchCoverUrl(apiKey, gameId);
    if (coverUrl.length === 0) {
      return { ok: false, coverPath: '', detail: `${matched.name} 没有竖版封面` };
    }
    const downloaded = await this.download(coverUrl, gameDirectory);
    if (!downloaded) {
      return { ok: false, coverPath: '', detail: '封面下载失败 (检查网络)' };
    }
    hilog.info(DOMAIN, TAG, 'cover saved %{public}s -> %{public}s', matched.name, downloaded);
    return { ok: true, coverPath: downloaded, detail: matched.name };
  }

  /** Validate and atomically replace a cover in the game's directory. */
  async download(url: string, gameDirectory: string): Promise<string> {
    const request = http.createHttp();
    let temporary = '';
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 15000,
        readTimeout: 30000
      });
      if (response.responseCode !== 200) {
        hilog.warn(DOMAIN, TAG, 'download http %{public}d for %{public}s', response.responseCode, url);
        return '';
      }
      const buffer = response.result as ArrayBuffer;
      if (!buffer || buffer.byteLength < 16 || buffer.byteLength > 12 * 1024 * 1024) return '';
      const bytes = new Uint8Array(buffer);
      const extension = bytes[0] === 0xFF && bytes[1] === 0xD8 ? '.jpg' :
        (bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4E && bytes[3] === 0x47 ? '.png' :
          (bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46 &&
            bytes[8] === 0x57 && bytes[9] === 0x45 && bytes[10] === 0x42 && bytes[11] === 0x50 ? '.webp' : ''));
      if (!extension) return '';
      const directory = gameDirectory.replace(/\/+$/, '');
      const destPath = directory + '/cover' + extension;
      temporary = destPath + '.tmp';
      const file = fs.openSync(temporary,
        fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        if (fs.writeSync(file.fd, buffer) !== buffer.byteLength) return '';
      } finally {
        fs.closeSync(file);
      }
      fs.renameSync(temporary, destPath);
      temporary = '';
      for (const other of ['cover.png', 'cover.jpg', 'cover.jpeg', 'cover.webp']) {
        if (other !== 'cover' + extension) {
          try { fs.unlinkSync(directory + '/' + other); } catch (_) {}
        }
      }
      return destPath;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'download failed: %{public}s', JSON.stringify(e));
      return '';
    } finally {
      request.destroy();
      if (temporary) try { fs.unlinkSync(temporary); } catch (_) {}
    }
  }

  /** 带鉴权头的 GET, 返回响应文本 (失败返回空串)。 */
  private async getJson(url: string, apiKey: string): Promise<string> {
    const request = http.createHttp();
    const headers: JsonHeaders = { Accept: 'application/json' };
    if (apiKey.trim()) headers.Authorization = `Bearer ${apiKey.trim()}`;
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        header: headers,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode !== 200) {
        hilog.warn(DOMAIN, TAG, 'api http %{public}d: %{public}s', response.responseCode, url);
        return '';
      }
      return response.result as string;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'api failed: %{public}s', JSON.stringify(e));
      return '';
    } finally {
      request.destroy();
    }
  }
}
