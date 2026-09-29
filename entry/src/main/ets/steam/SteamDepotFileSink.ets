import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '../service/LogService';
import { AppCatalogService } from '../service/AppCatalogService';
import { DepotFileSink } from './SteamDownloadService';

const DOMAIN = 0x0000;
const TAG = 'SteamDepotSink';

/**
 * Depot 落盘实现 (W6): 写 <Download库>/games/<安装名>/tmp/<depotId>/<相对路径>。
 * 按 manifest 全部 depot 写完后由调用方把 tmp 提升为正式目录并触发重扫描。
 */
export class SteamDepotWriter implements DepotFileSink {
  private baseDir: string = '';

  /** 初始化: 建 games/<name>/tmp/<depotId>。失败抛错。 */
  async begin(gameFolderName: string, depotId: number): Promise<void> {
    const gamesDir = AppCatalogService.getInstance().getGamesDirectory();
    if (!gamesDir || gamesDir.length === 0) {
      throw new Error('下载目录未就绪 (未授权 Download 目录?)');
    }
    this.baseDir = this.join(gamesDir, this.join(this.sanitize(gameFolderName), 'tmp/' + depotId));
    await this.ensureDirectory('');
  }

  base(): string {
    return this.baseDir;
  }

  async ensureDirectory(relativePath: string): Promise<void> {
    const target = relativePath.length > 0 ? this.join(this.baseDir, relativePath) : this.baseDir;
    try {
      await fs.mkdir(target, true);
    } catch (e) {
      // 已存在 (EEXIST) 不算失败
      const text = JSON.stringify(e);
      if (text.indexOf('13900008') < 0 && text.indexOf('exist') < 0) {
        hilog.warn(DOMAIN, TAG, 'mkdir %{public}s failed: %{public}s', target, text);
        throw new Error('创建目录失败: ' + relativePath);
      }
    }
  }

  async writeFile(relativePath: string, content: Uint8Array): Promise<void> {
    const normalized = relativePath.replace(/\\/g, '/');
    const slash = normalized.lastIndexOf('/');
    if (slash > 0) {
      await this.ensureDirectory(normalized.substring(0, slash));
    }
    const target = this.join(this.baseDir, normalized);
    const buffer = content.buffer.slice(content.byteOffset,
      content.byteOffset + content.byteLength) as ArrayBuffer;
    const file = fs.openSync(target, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    try {
      fs.writeSync(file.fd, buffer);
    } finally {
      fs.closeSync(file);
    }
  }

  private join(base: string, part: string): string {
    return base.replace(/\/+$/, '') + '/' + part.replace(/^\/+/, '');
  }

  private sanitize(name: string): string {
    const cleaned = name.replace(/[\\/:*?"<>|]/g, '_').trim();
    return cleaned.length > 0 ? cleaned : 'steam-game';
  }
}
