import { fileIo as fs } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { image } from '@kit.ImageKit';
import { LinuxSteamGame, validLinuxSteamAppId } from '../model/LinuxSteamModels';

const ROOT = '/data/storage/el2/base/files/linux-steam-artwork';
const LIMIT = 4 * 1024 * 1024;
export type LinuxSteamArtworkListener = () => void;

// Public capsule art only. No Steam login data or Windows Steam files are read.
export class LinuxSteamArtworkService {
  private static instance: LinuxSteamArtworkService | null = null;
  private queue: string[] = [];
  private pending: Set<string> = new Set<string>();
  private retryAt: Map<string, number> = new Map<string, number>();
  private listeners: Set<LinuxSteamArtworkListener> = new Set<LinuxSteamArtworkListener>();
  private active: number = 0;
  static getInstance(): LinuxSteamArtworkService {
    if (!LinuxSteamArtworkService.instance) LinuxSteamArtworkService.instance = new LinuxSteamArtworkService();
    return LinuxSteamArtworkService.instance;
  }
  subscribe(listener: LinuxSteamArtworkListener): void { this.listeners.add(listener); }
  unsubscribe(listener: LinuxSteamArtworkListener): void { this.listeners.delete(listener); }
  resolve(id: string, original: string): string {
    if (!validLinuxSteamAppId(id)) return '';
    if (original.startsWith('/')) return 'file://' + original;
    const path = `${ROOT}/${id}.jpg`;
    try { if (fs.statSync(path).size > 16) return 'file://' + path; } catch (_) {}
    return '';
  }
  ensure(games: LinuxSteamGame[]): void {
    for (const game of games) {
      if (!validLinuxSteamAppId(game.appId) || this.resolve(game.appId, game.cover) || this.pending.has(game.appId) ||
        (this.retryAt.get(game.appId) ?? 0) > Date.now()) continue;
      this.pending.add(game.appId); this.queue.push(game.appId);
    }
    this.pump();
  }
  private pump(): void {
    while (this.active < 2 && this.queue.length) {
      const id = this.queue.shift(); if (!id) break;
      this.active++;
      void this.download(id).finally((): void => {
        this.active--; this.pending.delete(id); this.pump();
      });
    }
  }
  private async download(id: string): Promise<void> {
    try { fs.mkdirSync(ROOT, true); } catch (_) {}
    const urls = [
      `https://cdn.cloudflare.steamstatic.com/steam/apps/${id}/library_600x900.jpg`,
      `https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/${id}/library_600x900.jpg`
    ];
    for (const url of urls) {
      const request = http.createHttp();
      const temp = `${ROOT}/${id}.tmp`;
      let source: image.ImageSource | null = null;
      try {
        const response = await request.request(url, { method: http.RequestMethod.GET,
          expectDataType: http.HttpDataType.ARRAY_BUFFER, connectTimeout: 8000, readTimeout: 15000,
          maxLimit: LIMIT });
        if (response.responseCode !== 200) continue;
        const buffer = response.result as ArrayBuffer;
        if (!buffer || buffer.byteLength < 16 || buffer.byteLength > LIMIT) continue;
        const bytes = new Uint8Array(buffer);
        if (bytes[0] !== 0xff || bytes[1] !== 0xd8) continue;
        source = image.createImageSource(buffer);
        const info = await source.getImageInfo();
        if (info.size.width < 100 || info.size.height < 100 || info.size.width * info.size.height > 8388608) continue;
        const file = fs.openSync(temp, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
        let written = 0;
        try { written = fs.writeSync(file.fd, buffer); } finally { fs.closeSync(file); }
        if (written !== buffer.byteLength) continue;
        fs.renameSync(temp, `${ROOT}/${id}.jpg`);
        this.listeners.forEach((listener: LinuxSteamArtworkListener): void => listener());
        return;
      } catch (_) { /* Try the second official CDN; keep the painted fallback visible. */ }
      finally {
        request.destroy();
        if (source) await source.release();
        try { fs.unlinkSync(temp); } catch (_) {}
      }
    }
    this.retryAt.set(id, Date.now() + 300000);
  }
}
