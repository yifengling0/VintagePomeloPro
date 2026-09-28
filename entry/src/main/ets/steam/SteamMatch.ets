import { http } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '../service/LogService';
import { SteamLibraryGame } from './vendor/models/SteamModels';

const DOMAIN = 0x0000;
const TAG = 'SteamMatch';

/** 竖版封面直链（storesearch 链路同一目标, 这里直接用 GetOwnedGames 的 appid）。 */
const CAPSULE_600X900_BASE = 'https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/';

/**
 * Steam 库 ↔ 本地目录匹配（W4.3）。
 * 名称规范化后双向包含；封面下载 library_600x900 优先, logoUrl 兜底。
 */
export class SteamMatch {
  /** 去年份/版本号/全角转半角/去括注/去分隔符, 留字母数字与 CJK。 */
  static normalizeTitle(title: string): string {
    let text = title.toLowerCase();
    let half = '';
    for (const ch of text) {
      const code = ch.charCodeAt(0);
      if (code === 0x3000) {
        half += ' ';
      } else if (code >= 0xFF01 && code <= 0xFF5E) {
        half += String.fromCharCode(code - 0xFEE0);
      } else {
        half += ch;
      }
    }
    text = half;
    // 去括注段: (..) [..] 【..】 （..）
    text = text.replace(/([([][^)\]]*[)\]]|【[^】]*】)/g, ' ');
    // 去版本号/年份/常见版本词
    text = text.replace(/\bv?\d+(\.\d+)+\b/g, ' ');
    text = text.replace(/\b(19|20)\d{2}\b/g, ' ');
    text = text.replace(/\b(goty|goty_|enhanced|definitive|remastered|remaster|deluxe|ultimate|edition|hd|final cut|directors cut|director's cut|game of the year)\b/g, ' ');
    text = text.replace(/(重制版|年度版|豪华版|增强版|终极版|典藏版|年度黄金版)/g, ' ');
    // 只留字母数字/CJK
    text = text.replace(/[^a-z0-9\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]+/g, '');
    return text.trim();
  }

  /** 双向包含匹配（规范化后任一方包含另一方, 且较短一方长度 ≥ 2）。 */
  static titlesMatch(localTitle: string, steamName: string): boolean {
    const a = SteamMatch.normalizeTitle(localTitle);
    const b = SteamMatch.normalizeTitle(steamName);
    if (a.length < 2 || b.length < 2) return false;
    return a.indexOf(b) >= 0 || b.indexOf(a) >= 0;
  }

  /** 双向包含匹配（规范化后任一方包含另一方, 且较短一方长度 ≥ 2）。 */
  static bestMatch(localTitle: string, games: Array<SteamLibraryGame>): SteamLibraryGame | null {
    for (const game of games) {
      if (SteamMatch.titlesMatch(localTitle, game.name)) {
        return game;
      }
    }
    return null;
  }

  /** 下载竖版封面写 dir/cover.jpg；先 library_600x900, 失败退 logoUrl。 */
  static async downloadCoverTo(appId: string, logoUrl: string, dir: string): Promise<boolean> {
    if (dir.length === 0) return false;
    const primary = CAPSULE_600X900_BASE + appId + '/library_600x900.jpg';
    let bytes = await SteamMatch.fetchBytes(primary);
    if (bytes === null && logoUrl.length > 0 && logoUrl !== primary) {
      bytes = await SteamMatch.fetchBytes(logoUrl);
    }
    if (bytes === null || bytes.byteLength < 2048) {
      // 过小的响应多半是错误页, 不当封面
      return false;
    }
    try {
      const file = fs.openSync(dir + '/cover.jpg',
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, bytes);
      fs.closeSync(file.fd);
      return true;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'cover write failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  private static async fetchBytes(url: string): Promise<ArrayBuffer | null> {
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode !== 200) return null;
      return response.result as ArrayBuffer;
    } catch (_) {
      return null;
    } finally {
      request.destroy();
    }
  }
}
