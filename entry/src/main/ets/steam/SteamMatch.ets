import { http } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '../service/LogService';
import { chooseCoverFileName } from '../model/AppCatalogRules';
import { normalizeGameTitle } from '../model/GameArtworkRules';
import { SteamLibraryGame } from './vendor/models/SteamModels';

const DOMAIN = 0x0000;
const TAG = 'SteamMatch';
const CAPSULE_600X900_BASE = 'https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/';

/** Match library identities exactly; preserve sequel numbers and edition names. */
export class SteamMatch {
  static normalizeTitle(title: string): string {
    let half = '';
    for (const ch of title) {
      const code = ch.charCodeAt(0);
      half += code === 0x3000 ? ' ' : (code >= 0xFF01 && code <= 0xFF5E
        ? String.fromCharCode(code - 0xFEE0) : ch);
    }
    return normalizeGameTitle(half);
  }

  static titlesMatch(localTitle: string, steamName: string): boolean {
    const expected = SteamMatch.normalizeTitle(localTitle);
    return expected.length > 0 && expected === SteamMatch.normalizeTitle(steamName);
  }

  static bestMatch(localTitle: string, games: Array<SteamLibraryGame>): SteamLibraryGame | null {
    let match: SteamLibraryGame | null = null;
    for (const game of games) {
      if (!SteamMatch.titlesMatch(localTitle, game.name)) continue;
      if (match && match.appId !== game.appId) return null;
      match = game;
    }
    return match;
  }

  /** Fill a missing cover only. Recheck manual selection and directory files after downloading. */
  static async downloadCoverTo(appId: string, logoUrl: string, dir: string,
    canWrite: () => boolean): Promise<boolean> {
    if (dir.length === 0 || !/^\d+$/.test(appId) || !canWrite()) return false;
    const directory = dir.replace(/\/+$/, '');
    let temporary = '';
    try {
      if (chooseCoverFileName(fs.listFileSync(directory))) return false;
      const primary = CAPSULE_600X900_BASE + appId + '/library_600x900.jpg';
      let bytes = await SteamMatch.fetchBytes(primary);
      if (bytes === null && logoUrl.length > 0 && logoUrl !== primary) {
        bytes = await SteamMatch.fetchBytes(logoUrl);
      }
      if (bytes === null) return false;
      const extension = SteamMatch.imageExtension(bytes);
      if (!extension) return false;
      const destination = directory + '/cover' + extension;
      temporary = destination + '.steam-' + Date.now() + '.tmp';
      const file = fs.openSync(temporary, fs.OpenMode.WRITE_ONLY | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      try {
        if (fs.writeSync(file.fd, bytes) !== bytes.byteLength) return false;
      } finally {
        fs.closeSync(file);
      }
      // No await between the final guard and rename: UI changes cannot interleave here.
      if (!canWrite() || chooseCoverFileName(fs.listFileSync(directory))) return false;
      fs.renameSync(temporary, destination);
      temporary = '';
      return true;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'cover write failed: %{public}s', JSON.stringify(e));
      return false;
    } finally {
      if (temporary) try { fs.unlinkSync(temporary); } catch (_) {}
    }
  }

  private static imageExtension(buffer: ArrayBuffer): string {
    if (buffer.byteLength < 16 || buffer.byteLength > 12 * 1024 * 1024) return '';
    const bytes = new Uint8Array(buffer);
    if (bytes[0] === 0xFF && bytes[1] === 0xD8 && bytes[2] === 0xFF) return '.jpg';
    if (bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4E && bytes[3] === 0x47 &&
      bytes[4] === 0x0D && bytes[5] === 0x0A && bytes[6] === 0x1A && bytes[7] === 0x0A) return '.png';
    if (bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46 &&
      bytes[8] === 0x57 && bytes[9] === 0x45 && bytes[10] === 0x42 && bytes[11] === 0x50) return '.webp';
    return '';
  }

  private static async fetchBytes(url: string): Promise<ArrayBuffer | null> {
    if (!url.startsWith('https://')) return null;
    const request = http.createHttp();
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        maxLimit: 12 * 1024 * 1024,
        connectTimeout: 15000,
        readTimeout: 20000
      });
      if (response.responseCode !== 200) return null;
      const bytes = response.result as ArrayBuffer;
      return bytes && SteamMatch.imageExtension(bytes) ? bytes : null;
    } catch (_) {
      return null;
    } finally {
      request.destroy();
    }
  }
}