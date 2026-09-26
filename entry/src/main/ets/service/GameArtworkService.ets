import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import preferences from '@ohos.data.preferences';
import { AppDescriptor, AppSource, GameBackgroundMode, cloneAppSettings } from '../model/AppModels';
import { chooseBackgroundFileName } from '../model/AppCatalogRules';
import { steamSearchLocales, uniqueArtworkGameId } from '../model/GameArtworkRules';
import { AppSettingsStore } from './AppSettingsStore';
import { hilog } from './LogService';

const DOMAIN = 0x0000;
const TAG = 'GameArtwork';
const MAX_IMAGE_BYTES = 12 * 1024 * 1024;
const RETRY_AFTER_MS = 60 * 60 * 1000;
const STATE_KEY = 'background_artwork_v1';

interface ArtworkRecord {
  appId: string;
  path: string;
  signature: string;
  outcome: string;
  attemptedAt: number;
}

interface ArtworkJob {
  app: AppDescriptor;
  force: boolean;
}

interface SteamSearchItem { id: number; name: string; }
interface SteamSearchResponse { items: SteamSearchItem[]; }
interface SgdbSearchItem { id: number; name: string; }
interface SgdbSearchResponse { success: boolean; data: SgdbSearchItem[]; }
interface SgdbHeroItem { url: string; width: number; height: number; }
interface SgdbHeroResponse { success: boolean; data: SgdbHeroItem[]; }
interface JsonHeaders { Authorization?: string; Accept: string; }
interface ArtworkLookup { url: string; transient: boolean; }
type DownloadOutcome = 'success' | 'no_match' | 'temporary' | 'superseded';

export type ArtworkListener = (appId: string, path: string) => void;

/** Resolves local artwork before scheduling a bounded, on-demand network lookup. */
export class GameArtworkService {
  private static instance: GameArtworkService | null = null;
  private preferences: preferences.Preferences | null = null;
  private root: string = '';
  private records: Map<string, ArtworkRecord> = new Map<string, ArtworkRecord>();
  private queue: ArtworkJob[] = [];
  private queuedIds: Set<string> = new Set<string>();
  private active: number = 0;
  private listeners: Set<ArtworkListener> = new Set<ArtworkListener>();

  static getInstance(): GameArtworkService {
    if (!GameArtworkService.instance) GameArtworkService.instance = new GameArtworkService();
    return GameArtworkService.instance;
  }

  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (this.preferences) return;
    this.root = context.filesDir + '/game_artwork';
    try { fs.mkdirSync(this.root, true); } catch (_) {}
    this.preferences = await preferences.getPreferences(context, 'game_artwork');
    try {
      const raw = this.preferences.getSync(STATE_KEY, '[]') as string;
      const values = JSON.parse(raw) as ArtworkRecord[];
      if (Array.isArray(values)) values.forEach((item: ArtworkRecord): void => {
        if (item.appId) this.records.set(item.appId, item);
      });
    } catch (_) { this.records.clear(); }
  }

  subscribe(listener: ArtworkListener): void { this.listeners.add(listener); }
  unsubscribe(listener: ArtworkListener): void { this.listeners.delete(listener); }

  private notify(app: AppDescriptor): void {
    const path = this.resolve(app);
    this.listeners.forEach((listener: ArtworkListener): void => listener(app.id, path));
  }

  private exists(path: string): boolean {
    if (!path) return false;
    try { return fs.accessSync(path); } catch (_) { return false; }
  }

  private artworkMatchesTitle(app: AppDescriptor, record: ArtworkRecord): boolean {
    return (record.signature ?? '').startsWith(app.title.trim().toLowerCase() + '#');
  }

  /** Manual mode stays locked even if its image was removed outside the app. */
  resolve(app: AppDescriptor): string {
    const settings = AppSettingsStore.getInstance().getAppSettings(app.id);
    if (settings.backgroundMode === GameBackgroundMode.MANUAL) {
      return this.exists(settings.manualBackgroundPath) ? settings.manualBackgroundPath : '';
    }
    if (this.exists(app.backgroundPath ?? '')) return app.backgroundPath ?? '';
    const saved = this.records.get(app.id);
    return saved && this.artworkMatchesTitle(app, saved) && this.exists(saved.path) ? saved.path : '';
  }

  sourceLabel(app: AppDescriptor): string {
    const settings = AppSettingsStore.getInstance().getAppSettings(app.id);
    if (settings.backgroundMode === GameBackgroundMode.MANUAL) {
      return this.exists(settings.manualBackgroundPath) ? '手动设置' : '手动图片丢失（自动获取仍暂停）';
    }
    if (this.exists(app.backgroundPath ?? '')) return '游戏目录';
    const saved = this.records.get(app.id);
    if (saved && this.artworkMatchesTitle(app, saved) && this.exists(saved.path)) return '自动获取';
    return '尚无背景';
  }

  async importManual(app: AppDescriptor, uri: string): Promise<boolean> {
    if (!this.root || !uri) return false;
    const prefix = this.root + '/manual-' + app.id + '-' + Date.now().toString();
    const temporary = prefix + '.tmp';
    let path = '';
    let fd = -1;
    try {
      fd = fs.openSync(uri, fs.OpenMode.READ_ONLY).fd;
      await fs.copyFile(fd, temporary);
      const size = fs.statSync(temporary).size;
      if (size < 16 || size > MAX_IMAGE_BYTES) return false;
      const input = fs.openSync(temporary, fs.OpenMode.READ_ONLY);
      const header = new ArrayBuffer(16);
      try { fs.readSync(input.fd, header, { length: 16 }); }
      finally { fs.closeSync(input); }
      const extension = this.imageExtension(new Uint8Array(header));
      if (!extension) return false;
      path = prefix + extension;
      fs.renameSync(temporary, path);
      const store = AppSettingsStore.getInstance();
      const next = cloneAppSettings(store.getAppSettings(app.id));
      const previous = next.manualBackgroundPath;
      next.backgroundMode = GameBackgroundMode.MANUAL;
      next.manualBackgroundPath = path;
      await store.saveAppSettings(next);
      if (previous && previous !== path && previous.startsWith(this.root + '/manual-')) {
        try { fs.unlinkSync(previous); } catch (_) {}
      }
      this.notify(app);
      return true;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'manual import failed: %{public}s', JSON.stringify(error));
      return false;
    } finally {
      try { fs.unlinkSync(temporary); } catch (_) {}
      if (path && AppSettingsStore.getInstance().getAppSettings(app.id).manualBackgroundPath !== path) {
        try { fs.unlinkSync(path); } catch (_) {}
      }
      if (fd >= 0) try { fs.closeSync(fd); } catch (_) {}
    }
  }

  async restoreAutomatic(app: AppDescriptor): Promise<void> {
    const store = AppSettingsStore.getInstance();
    const next = cloneAppSettings(store.getAppSettings(app.id));
    const previous = next.manualBackgroundPath;
    next.backgroundMode = GameBackgroundMode.AUTO;
    next.manualBackgroundPath = '';
    await store.saveAppSettings(next);
    if (previous && previous.startsWith(this.root + '/manual-')) {
      try { fs.unlinkSync(previous); } catch (_) {}
    }
    this.notify(app);
    this.enqueueIfMissing(app);
  }

  private signature(app: AppDescriptor): string {
    const key = AppSettingsStore.getInstance().getSgdbApiKey();
    // v3 retries records produced before localized lookup and safe edition aliases.
    if (!key) return app.title.trim().toLowerCase() + '#steam-v3';
    let hash = 2166136261;
    for (let index = 0; index < key.length; index++) {
      hash ^= key.charCodeAt(index);
      hash = Math.imul(hash, 16777619);
    }
    return app.title.trim().toLowerCase() + '#sgdb-v3-' + (hash >>> 0).toString(16);
  }

  enqueueIfMissing(app: AppDescriptor): void { this.enqueue(app, false); }
  retryNow(app: AppDescriptor): void { this.enqueue(app, true); }

  private enqueue(app: AppDescriptor, force: boolean): void {
    if (!this.preferences || app.source !== AppSource.DOWNLOAD || !app.title.trim()) return;
    if (AppSettingsStore.getInstance().getAppSettings(app.id).backgroundMode === GameBackgroundMode.MANUAL) return;
    if (this.exists(app.backgroundPath ?? '')) return;
    if (!force && this.resolve(app)) return;
    if (this.queuedIds.has(app.id)) return;
    const record = this.records.get(app.id);
    const signature = this.signature(app);
    if (!force && record && record.signature === signature) {
      if (record.outcome === 'no_match') return;
      if (record.outcome === 'temporary' && Date.now() - record.attemptedAt < RETRY_AFTER_MS) return;
    }
    this.queuedIds.add(app.id);
    this.queue.push({ app: app, force: force });
    this.pump();
  }

  private pump(): void {
    while (this.active < 2 && this.queue.length > 0) {
      const job = this.queue.shift();
      if (!job) break;
      this.active++;
      void this.process(job).finally((): void => {
        this.active--;
        this.queuedIds.delete(job.app.id);
        this.pump();
      });
    }
  }

  private async process(job: ArtworkJob): Promise<void> {
    const app = job.app;
    const signature = this.signature(app);
    let outcome = 'no_match';
    try {
      const steam = await this.findSteamBackgroundUrl(app.title);
      outcome = steam.transient ? 'temporary' : 'no_match';
      if (steam.url) {
        const downloaded = await this.downloadAndCommit(app, steam.url);
        if (downloaded === 'success') outcome = 'success';
        else if (downloaded === 'superseded') return;
        else outcome = downloaded;
      }
      if (outcome !== 'success' && AppSettingsStore.getInstance().getSgdbApiKey()) {
        const sgdb = await this.findSgdbBackgroundUrl(app.title);
        if (sgdb.url) {
          const downloaded = await this.downloadAndCommit(app, sgdb.url);
          if (downloaded === 'superseded') return;
          outcome = downloaded;
        } else if (sgdb.transient) {
          outcome = 'temporary';
        }
      }
    } catch (error) {
      outcome = 'temporary';
      hilog.warn(DOMAIN, TAG, 'lookup failed: %{public}s', JSON.stringify(error));
    }
    if (AppSettingsStore.getInstance().getAppSettings(app.id).backgroundMode === GameBackgroundMode.MANUAL ||
      await this.hasDirectoryBackground(app.directory)) return;
    const previous = this.records.get(app.id);
    const retainedPath = previous && this.artworkMatchesTitle(app, previous) ? previous.path : '';
    this.records.set(app.id, {
      appId: app.id, path: retainedPath, signature: signature,
      outcome: outcome, attemptedAt: Date.now()
    });
    if (previous?.path && !retainedPath && previous.path.startsWith(this.root + '/auto-')) {
      try { fs.unlinkSync(previous.path); } catch (_) {}
    }
    try { await this.saveRecords(); }
    catch (error) {
      hilog.warn(DOMAIN, TAG, 'artwork state save failed: %{public}s', JSON.stringify(error));
    }
    this.notify(app);
  }

  private async findSteamBackgroundUrl(title: string): Promise<ArtworkLookup> {
    let transient = false;
    const locales = steamSearchLocales(title);
    for (let index = 0; index < locales.length; index++) {
      try {
        const url = 'https://store.steampowered.com/api/storesearch/?term=' +
          encodeURIComponent(title) + '&l=' + locales[index] + '&cc=US';
        const parsed = JSON.parse(await this.getJson(url, '')) as SteamSearchResponse;
        const gameId = uniqueArtworkGameId(parsed.items ?? [], title);
        if (gameId > 0) {
          return { url: 'https://cdn.cloudflare.steamstatic.com/steam/apps/' +
            gameId.toString() + '/library_hero.jpg', transient: false };
        }
      } catch (_) { transient = true; }
    }
    return { url: '', transient: transient };
  }

  private async findSgdbBackgroundUrl(title: string): Promise<ArtworkLookup> {
    const key = AppSettingsStore.getInstance().getSgdbApiKey();
    if (!key) return { url: '', transient: false };
    try {
      const search = JSON.parse(await this.getJson('https://www.steamgriddb.com/api/v2/search/autocomplete/' +
        encodeURIComponent(title), key)) as SgdbSearchResponse;
      const gameId = uniqueArtworkGameId(search.data ?? [], title);
      if (!search.success || gameId <= 0) return { url: '', transient: false };
      const heroes = JSON.parse(await this.getJson('https://www.steamgriddb.com/api/v2/heroes/game/' +
        gameId.toString(), key)) as SgdbHeroResponse;
      if (!heroes.success || !heroes.data) return { url: '', transient: false };
      const candidates = heroes.data.filter((item: SgdbHeroItem): boolean =>
        item.height > 0 && item.width / item.height >= 1.4 &&
        (item.url.startsWith('https://cdn2.steamgriddb.com/') ||
          item.url.startsWith('https://cdn.steamgriddb.com/')));
      candidates.sort((a: SgdbHeroItem, b: SgdbHeroItem): number =>
        b.width * b.height - a.width * a.height);
      return { url: candidates.length > 0 ? candidates[0].url : '', transient: false };
    } catch (_) { return { url: '', transient: true }; }
  }

  private async getJson(url: string, key: string): Promise<string> {
    const request = http.createHttp();
    const headers: JsonHeaders = { Accept: 'application/json' };
    if (key) headers.Authorization = 'Bearer ' + key;
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        header: headers,
        expectDataType: http.HttpDataType.STRING,
        connectTimeout: 12000, readTimeout: 15000
      });
      if (response.responseCode !== 200) throw new Error('HTTP ' + response.responseCode.toString());
      return response.result as string;
    } finally { request.destroy(); }
  }

  private imageExtension(bytes: Uint8Array): string {
    if (bytes.length < 16) return '';
    if (bytes[0] === 0xFF && bytes[1] === 0xD8) return '.jpg';
    if (bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4E && bytes[3] === 0x47) return '.png';
    if (bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46 &&
      bytes[8] === 0x57 && bytes[9] === 0x45 && bytes[10] === 0x42 && bytes[11] === 0x50) return '.webp';
    if (bytes[0] === 0x42 && bytes[1] === 0x4D) return '.bmp';
    return '';
  }

  private async hasDirectoryBackground(directory: string): Promise<boolean> {
    if (!directory) return false;
    try { return chooseBackgroundFileName(await fs.listFile(directory)) !== ''; }
    catch (_) { return false; }
  }

  private async downloadAndCommit(app: AppDescriptor, url: string): Promise<DownloadOutcome> {
    const request = http.createHttp();
    let temp = '';
    try {
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 12000, readTimeout: 25000
      });
      if (response.responseCode !== 200) {
        return response.responseCode === 404 ? 'no_match' : 'temporary';
      }
      const buffer = response.result as ArrayBuffer;
      if (!buffer || buffer.byteLength < 16 || buffer.byteLength > MAX_IMAGE_BYTES) return 'no_match';
      const extension = this.imageExtension(new Uint8Array(buffer));
      if (!extension) return 'no_match';
      if (AppSettingsStore.getInstance().getAppSettings(app.id).backgroundMode === GameBackgroundMode.MANUAL ||
        await this.hasDirectoryBackground(app.directory)) return 'superseded';
      const dest = this.root + '/auto-' + app.id + '-' + Date.now().toString() + extension;
      temp = dest + '.tmp';
      const file = fs.openSync(temp, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      let written = 0;
      try { written = fs.writeSync(file.fd, buffer); } finally { fs.closeSync(file); }
      if (written !== buffer.byteLength || fs.statSync(temp).size !== buffer.byteLength) return 'temporary';
      if (AppSettingsStore.getInstance().getAppSettings(app.id).backgroundMode === GameBackgroundMode.MANUAL ||
        await this.hasDirectoryBackground(app.directory)) return 'superseded';
      fs.renameSync(temp, dest);
      temp = '';
      const previous = this.records.get(app.id);
      this.records.set(app.id, {
        appId: app.id, path: dest, signature: this.signature(app), outcome: 'success', attemptedAt: Date.now()
      });
      if (previous?.path && previous.path !== dest && previous.path.startsWith(this.root + '/auto-')) {
        try { fs.unlinkSync(previous.path); } catch (_) {}
      }
      return 'success';
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'download failed: %{public}s', JSON.stringify(error));
      return 'temporary';
    } finally {
      request.destroy();
      if (temp) try { fs.unlinkSync(temp); } catch (_) {}
    }
  }

  private async saveRecords(): Promise<void> {
    if (!this.preferences) return;
    const values: ArtworkRecord[] = [];
    this.records.forEach((value: ArtworkRecord): void => { values.push(value); });
    await this.preferences.put(STATE_KEY, JSON.stringify(values));
    await this.preferences.flush();
  }
}
