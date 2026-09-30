import { fileIo as fs } from '@kit.CoreFileKit';
import { AppCatalogService } from '../service/AppCatalogService';
import { DepotFileSink } from './SteamDownloadService';

/** Writes an app into a private staging directory, then publishes it atomically. */
export class SteamDepotWriter implements DepotFileSink {
  private stagingDir: string = '';
  private finalDir: string = '';
  private title: string = '';
  private appId: number = 0;

  async begin(appId: number, title: string): Promise<void> {
    if (!Number.isSafeInteger(appId) || appId <= 0) throw new Error('Invalid Steam App ID');
    const catalog = AppCatalogService.getInstance();
    if (!catalog.isGamesDirectoryReady()) throw new Error('游戏目录尚未就绪');
    const libraryRoot = catalog.getLibraryRoot();
    const gamesDir = catalog.getGamesDirectory();
    if (!libraryRoot || !gamesDir) throw new Error('Steam 下载目录不可用');
    this.appId = appId;
    this.title = title.trim().slice(0, 200) || ('Steam ' + appId);
    // The Download provider grants writes below games/, but may reject new
    // siblings of games/. The catalog explicitly ignores this staging folder.
    const stagingRoot = gamesDir + '/steam-download-staging';
    this.makeDirectory(stagingRoot);
    this.stagingDir = stagingRoot + '/app-' + appId + '-' + Date.now() + '-' +
      Math.floor(Math.random() * 1000000);
    this.finalDir = gamesDir + '/steam/' + appId;
    if (this.exists(this.finalDir)) throw new Error('Steam 游戏目录已存在，请先在游戏库中检查');
    this.makeDirectory(this.stagingDir);
  }

  base(): string { return this.finalDir; }

  private safePath(relativePath: string): string {
    if (!this.stagingDir) throw new Error('Steam 下载尚未初始化');
    const normalized = relativePath.replace(/\\/g, '/');
    if (!normalized || normalized.startsWith('/') || /^[A-Za-z]:/.test(normalized) ||
      normalized.indexOf('\0') >= 0 || normalized.toLowerCase() === '.steam-game.json') {
      throw new Error('Steam manifest contains an invalid path');
    }
    const parts = normalized.split('/');
    for (const part of parts) {
      if (!part || part === '.' || part === '..' || /[\x00-\x1F:*?"<>|]/.test(part)) {
        throw new Error('Steam manifest contains an unsafe path');
      }
    }
    return this.stagingDir + '/' + parts.join('/');
  }

  private makeDirectory(path: string): void {
    if (this.exists(path)) {
      if (!fs.statSync(path).isDirectory()) throw new Error('Steam path is not a directory');
      return;
    }
    try {
      fs.mkdirSync(path);
    } catch (directError) {
      // Some document providers expose the path only through the recursive
      // fileIo route. The verified games/ parent already exists here.
      try { fs.mkdirSync(path, true); }
      catch (_) {
        if (!this.exists(path)) {
          throw new Error('Steam directory creation failed: ' + String(directError));
        }
      }
    }
    if (!fs.statSync(path).isDirectory()) throw new Error('Steam path is not a directory');
  }

  private exists(path: string): boolean {
    try { fs.statSync(path); return true; } catch (_) { return false; }
  }

  async ensureDirectory(relativePath: string): Promise<void> {
    const path = this.safePath(relativePath);
    const relative = path.substring(this.stagingDir.length + 1).split('/');
    let current = this.stagingDir;
    for (const part of relative) {
      current += '/' + part;
      this.makeDirectory(current);
    }
  }

  async prepareFile(relativePath: string, size: number): Promise<void> {
    if (!Number.isSafeInteger(size) || size < 0) throw new Error('Invalid Steam file size');
    const path = this.safePath(relativePath);
    const slash = relativePath.replace(/\\/g, '/').lastIndexOf('/');
    if (slash > 0) await this.ensureDirectory(relativePath.substring(0, slash));
    const file = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    try { fs.truncateSync(file.fd, size); } finally { fs.closeSync(file); }
  }

  async writeChunk(relativePath: string, offset: number, content: Uint8Array): Promise<void> {
    if (!Number.isSafeInteger(offset) || offset < 0) throw new Error('Invalid Steam chunk offset');
    const path = this.safePath(relativePath);
    const file = fs.openSync(path, fs.OpenMode.READ_WRITE);
    try {
      let done = 0;
      while (done < content.length) {
        const remaining = content.subarray(done);
        const buffer = remaining.buffer.slice(remaining.byteOffset,
          remaining.byteOffset + remaining.byteLength) as ArrayBuffer;
        const written = fs.writeSync(file.fd, buffer, { offset: offset + done });
        if (written <= 0) throw new Error('Steam file write stopped');
        done += written;
      }
    } finally { fs.closeSync(file); }
  }

  async finishFile(relativePath: string, size: number): Promise<void> {
    const path = this.safePath(relativePath);
    const file = fs.openSync(path, fs.OpenMode.READ_WRITE);
    try {
      if (fs.statSync(path).size !== size) throw new Error('Steam file size mismatch');
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
  }

  async publish(): Promise<string> {
    if (!this.stagingDir || !this.finalDir) throw new Error('Steam download is not ready');
    const marker = this.stagingDir + '/.steam-game.json';
    const file = fs.openSync(marker, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    try {
      fs.writeSync(file.fd, JSON.stringify({ appId: this.appId, name: this.title, source: 'steam' }));
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
    const steamRoot = this.finalDir.substring(0, this.finalDir.lastIndexOf('/'));
    this.makeDirectory(steamRoot);
    if (this.exists(this.finalDir)) throw new Error('Steam 游戏目录已存在，未覆盖现有文件');
    await fs.rename(this.stagingDir, this.finalDir);
    this.stagingDir = '';
    return this.finalDir;
  }

  async discard(): Promise<void> {
    const path = this.stagingDir;
    this.stagingDir = '';
    if (path && path.indexOf('/steam-download-staging/app-' + this.appId + '-') >= 0) {
      try { await fs.rmdir(path); } catch (_) {}
    }
  }
}
