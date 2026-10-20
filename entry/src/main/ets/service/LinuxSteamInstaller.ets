import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { LinuxSteamDeploymentPlan, LinuxSteamDownloadArchive, LinuxSteamInstallSnapshot, LinuxSteamInstallPhase,
  validateLinuxSteamDeployment, linuxSteamInstallBusy } from '../model/LinuxSteamDownloadModels';
import { LinuxSteamLibraryService } from './LinuxSteamLibraryService';
import { prepareLinuxSteamClientData, prepareLinuxSteamExternalStorage } from './LinuxSteamDiagnosticData';
import { ConfigurationFiles } from './ConfigurationFiles';
import { ensureSmokeDirectory, readSmokeText } from './ManagedSmokePayloadService';
import { LinuxSteamComponentDownload, LinuxSteamDownloadError } from './LinuxSteamComponentDownload';
import { reclaimLinuxSteamReleases } from './LinuxSteamReleaseRetention';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const STATE = `${BASE}/component-download.json`;
export type LinuxSteamInstallListener = (value: LinuxSteamInstallSnapshot) => void;
interface OfflinePackage { schemaVersion: number; id: string; bytes: number; sha256: string; fileName: string; }

function removeInstallationScratch(path: string): void {
  if (!fs.accessSync(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink()) throw new Error('安装临时目录包含符号链接');
  if (!stat.isDirectory()) { fs.unlinkSync(path); return; }
  for (const name of fs.listFileSync(path)) {
    if (!name || name === '.' || name === '..' || name.includes('/')) throw new Error('安装临时目录不正确');
    removeInstallationScratch(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}
function installationDirectory(path: string): void {
  const marker = path.indexOf('/SteamLinuxARM64');
  const boundary = marker >= 0 ? path.substring(0, marker + '/SteamLinuxARM64'.length) : BASE;
  if (path !== boundary && !path.startsWith(boundary + '/')) throw new Error('组件目录超出安装位置');
  const relative = path.substring(boundary.length).split('/').filter((part: string): boolean => part.length > 0);
  let current = boundary;
  for (const part of ['', ...relative]) {
    if (part) current += '/' + part;
    if (fs.accessSync(current)) {
      const stat = fs.lstatSync(current);
      if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('组件目录包含无效路径或符号链接');
    } else if (!ensureSmokeDirectory(current)) throw new Error('无法创建组件安装目录');
  }
}
function safeInstallationParents(root: string, file: string): boolean {
  if (!file.startsWith(root + '/')) return false;
  let current = root;
  for (const part of file.substring(root.length + 1).split('/').slice(0, -1)) {
    current += '/' + part;
    if (!fs.accessSync(current)) return false;
    const stat = fs.lstatSync(current);
    if (!stat.isDirectory() || stat.isSymbolicLink()) return false;
  }
  return true;
}

export class LinuxSteamInstallService {
  private static instance: LinuxSteamInstallService | null = null;
  private listeners: Set<LinuxSteamInstallListener> = new Set<LinuxSteamInstallListener>();
  private operation: Promise<void> | null = null;
  private transfer: LinuxSteamComponentDownload | null = null;
  private paused: boolean = false;
  private loaded: boolean = false;
  private lastPublish: number = 0;
  private speedAt: number = 0;
  private speedBase: number = 0;
  private plan: LinuxSteamDeploymentPlan | null = null;
  private step: string = '';
  private value: LinuxSteamInstallSnapshot = { phase: 'idle', message: '首次使用需要下载 Steam 运行组件',
    component: '', receivedBytes: 0, totalBytes: 0, speedBytes: 0, completedComponents: 0, totalComponents: 0,
    preparedFiles: 0, totalFiles: 0, requiredSpaceBytes: 0, errorCode: '', error: '' };
  static getInstance(): LinuxSteamInstallService {
    if (!LinuxSteamInstallService.instance) LinuxSteamInstallService.instance = new LinuxSteamInstallService();
    return LinuxSteamInstallService.instance;
  }
  installed(): boolean {
    const root = LinuxSteamLibraryService.getInstance().externalRoot();
    if (!root) return false;
    const id = readSmokeText(`${root}/active-version.txt`).trim();
    return /^[a-f0-9]{64}$/.test(id) && readSmokeText(`${root}/releases/${id}/.verified`).trim() === id;
  }
  snapshot(): LinuxSteamInstallSnapshot { return { phase: this.value.phase, message: this.value.message,
    component: this.value.component, receivedBytes: this.value.receivedBytes, totalBytes: this.value.totalBytes,
    speedBytes: this.value.speedBytes, completedComponents: this.value.completedComponents,
    totalComponents: this.value.totalComponents, preparedFiles: this.value.preparedFiles, totalFiles: this.value.totalFiles,
    requiredSpaceBytes: this.value.requiredSpaceBytes, errorCode: this.value.errorCode, error: this.value.error }; }
  subscribe(listener: LinuxSteamInstallListener): void { this.listeners.add(listener); listener(this.snapshot()); }
  unsubscribe(listener: LinuxSteamInstallListener): void { this.listeners.delete(listener); }
  private publish(force: boolean = true): void {
    if (!force && Date.now() - this.lastPublish < 200) return;
    this.lastPublish = Date.now();
    this.listeners.forEach((listener: LinuxSteamInstallListener): void => listener(this.snapshot()));
    if (force) { try { installationDirectory(BASE); ConfigurationFiles.json(STATE, JSON.stringify(this.snapshot())); } catch (_) {} }
  }
  async initialize(context: common.Context): Promise<void> {
    if (!this.loaded) {
      this.loaded = true;
      try {
        const previous = JSON.parse(readSmokeText(STATE)) as LinuxSteamInstallSnapshot;
        if (previous.phase === 'paused' || previous.phase === 'failed' || linuxSteamInstallBusy(previous.phase)) {
          this.value.phase = 'paused'; this.value.message = '上次安装未完成，点击继续即可恢复';
        }
      } catch (_) {}
    }
    if (!this.plan) {
      const bytes = await context.resourceManager.getRawFileContent('linux-steam/deployment.json');
      const plan = JSON.parse(ConfigurationFiles.text(bytes)) as LinuxSteamDeploymentPlan;
      validateLinuxSteamDeployment(plan);
      if (await ConfigurationFiles.hash(ConfigurationFiles.bytes(plan.releaseManifest)) !== plan.id) {
        throw new LinuxSteamDownloadError('MANIFEST', '组件清单校验失败，请更新应用');
      }
      this.plan = plan; this.value.totalBytes = plan.downloadBytes; this.value.requiredSpaceBytes = plan.requiredSpaceBytes;
      this.value.totalComponents = plan.archives.length; this.value.totalFiles = plan.files.length;
    }
    if (this.installed() && !this.operation) { this.value.phase = 'ready'; this.value.message = 'Steam 运行组件已就绪'; }
    else if (!this.installed() && this.value.phase === 'ready') { this.value.phase = 'idle'; this.value.receivedBytes = 0; }
    this.publish();
  }
  pause(): void {
    if (!this.operation) return;
    this.paused = true; this.transfer?.pause(); this.value.message = '正在暂停，已下载内容会保留'; this.publish();
  }
  async install(context: common.Context, force: boolean = false, uri: string = ''): Promise<void> {
    if (this.operation) return this.operation;
    this.paused = false;
    const task = this.performInstall(context, force, uri); this.operation = task;
    try { await task; } finally { if (this.operation === task) this.operation = null; this.transfer = null; }
  }
  async importLocal(context: common.Context, uri: string): Promise<void> {
    if (!uri) return;
    if (this.operation) throw new Error('请先暂停当前任务，再导入组件包');
    await this.install(context, true, uri);
  }
  isBusy(): boolean { return this.operation !== null; }
  private async importArchives(context: common.Context, plan: LinuxSteamDeploymentPlan, root: string,
    staging: string, uri: string): Promise<void> {
    const metadata = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent('linux-steam/offline-package.json'))) as OfflinePackage;
    if (metadata.schemaVersion !== 1 || metadata.id !== plan.id || !/^[a-f0-9]{64}$/.test(metadata.sha256) ||
      !Number.isSafeInteger(metadata.bytes) || metadata.bytes <= 0 || metadata.bytes > 536870912) {
      throw new LinuxSteamDownloadError('LOCAL_VERSION', '本地包配置不兼容，请更新应用');
    }
    if (await statfs.getFreeSize(root) < plan.requiredSpaceBytes + metadata.bytes * 2) {
      throw new LinuxSteamDownloadError('SPACE', '本地导入需要约 3.8 GB 可用空间，请清理空间后重试');
    }
    this.value.component = '本地组件包'; this.value.totalBytes = metadata.bytes;
    this.value.receivedBytes = 0; this.stage('installing', '正在读取本地组件包');
    const input = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    const archivePath = `${staging}/local-import.zip`;
    let output: fs.File | null = null;
    try {
      if (fs.statSync(input.fd).size !== metadata.bytes) throw new LinuxSteamDownloadError('LOCAL_PACKAGE', '组件包不适用于当前应用版本，请选择配套的 HmosDeck Steam 组件包');
      if (fs.accessSync(archivePath)) fs.unlinkSync(archivePath);
      output = fs.openSync(archivePath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
      const buffer = new ArrayBuffer(1048576); let copied = 0;
      while (copied < metadata.bytes) {
        this.checkPaused();
        const count = fs.readSync(input.fd, buffer, { length: Math.min(buffer.byteLength, metadata.bytes - copied) });
        if (count <= 0) throw new LinuxSteamDownloadError('LOCAL_READ', '组件包读取中断，请重新选择文件');
        let sent = 0;
        while (sent < count) { const n = fs.writeSync(output.fd, buffer.slice(sent, count));
          if (n <= 0) throw new Error('无法写入组件包'); sent += n; }
        copied += count; this.value.receivedBytes = copied; this.publish(false);
        await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });
      }
      fs.fsyncSync(output.fd);
    } finally { fs.closeSync(input); if (output) fs.closeSync(output); }
    this.stage('verifying', '正在验证本地组件包');
    if (await ConfigurationFiles.fileHash(archivePath) !== metadata.sha256) {
      fs.unlinkSync(archivePath);
      throw new LinuxSteamDownloadError('LOCAL_CHECKSUM', '组件包校验失败或版本不匹配，原有组件和游戏库未更改');
    }
    this.checkPaused();
    const unpacked = `${staging}/local-import`; removeInstallationScratch(unpacked); installationDirectory(unpacked);
    this.stage('installing', '正在提取本地组件包');
    // Whole-bundle hash is pinned in the signed HAP before any ZIP extraction.
    await zlib.decompressFile(archivePath, unpacked, {});
    for (const archive of plan.archives) {
      if (archive.url.startsWith('rawfile://')) continue;
      this.checkPaused();
      const source = `${unpacked}/archives/${archive.sha256}.zip`;
      if (fs.statSync(source).size !== archive.bytes || await ConfigurationFiles.fileHash(source) !== archive.sha256) {
        throw new LinuxSteamDownloadError('LOCAL_CHECKSUM', '本地包中的组件校验失败，未更改游戏库');
      }
      const destination = `${root}/downloads/${archive.sha256}.zip`;
      if (fs.accessSync(destination)) fs.unlinkSync(destination);
      fs.renameSync(source, destination);
    }
    removeInstallationScratch(unpacked); fs.unlinkSync(archivePath);
    this.value.totalBytes = plan.downloadBytes; this.value.receivedBytes = 0;
  }
  private checkPaused(): void { if (this.paused) throw new LinuxSteamDownloadError('PAUSED', '安装已暂停，点击继续即可恢复'); }
  private stage(phase: LinuxSteamInstallPhase, message: string): void {
    this.value.phase = phase; this.value.message = message; this.value.speedBytes = 0; this.publish();
  }
  private async embedded(context: common.Context, archive: LinuxSteamDownloadArchive, destination: string): Promise<void> {
    if (fs.accessSync(destination) && fs.statSync(destination).size === archive.bytes &&
      await ConfigurationFiles.fileHash(destination) === archive.sha256) return;
    const name = archive.url.substring('rawfile://'.length);
    const input = context.resourceManager.getRawFdSync(name);
    const temporary = destination + '.part';
    const output = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
    try {
      if (input.length !== archive.bytes) throw new Error('内置组件大小不正确，请重新安装应用');
      const buffer = new ArrayBuffer(1048576); let copied = 0;
      while (copied < input.length) {
        this.checkPaused();
        const count = fs.readSync(input.fd, buffer, { offset: input.offset + copied, length: Math.min(buffer.byteLength, input.length - copied) });
        if (count <= 0) throw new Error('无法读取内置组件');
        let sent = 0;
        while (sent < count) {
          const n = fs.writeSync(output.fd, buffer.slice(sent, count));
          if (n <= 0) throw new Error('无法写入内置组件'); sent += n;
        }
        copied += count;
        await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });
      }
      fs.fsyncSync(output.fd);
    } finally { fs.closeSync(output); context.resourceManager.closeRawFdSync(name); }
    if (await ConfigurationFiles.fileHash(temporary) !== archive.sha256) throw new Error('内置组件校验失败，请重新安装应用');
    if (fs.accessSync(destination)) fs.unlinkSync(destination);
    fs.renameSync(temporary, destination);
  }
  private async performInstall(context: common.Context, force: boolean, uri: string): Promise<void> {
    this.value.error = ''; this.value.errorCode = '';
    try {
      await this.initialize(context);
      if (this.installed() && !force) return;
      this.stage('checking', '正在检查存储空间和运行组件');
      const root = await prepareLinuxSteamExternalStorage(context); this.checkPaused();
      const plan = this.plan;
      if (!plan) throw new LinuxSteamDownloadError('MANIFEST', '未取得组件清单');
      if (await statfs.getFreeSize(root) < plan.requiredSpaceBytes) {
        throw new LinuxSteamDownloadError('SPACE', `存储空间不足，请至少预留 ${(plan.requiredSpaceBytes / 1073741824).toFixed(1)} GB 后重试`);
      }
      installationDirectory(`${root}/downloads`); installationDirectory(`${root}/staging`);
      const staging = `${root}/staging/${plan.id}`; installationDirectory(staging);
      this.value.receivedBytes = 0;
      let completed = 0; this.value.completedComponents = 0; this.value.preparedFiles = 0;
      this.transfer = new LinuxSteamComponentDownload();
      if (uri) await this.importArchives(context, plan, root, staging, uri);
      this.value.totalBytes = plan.downloadBytes;
      this.value.receivedBytes = 0;
      // Prepare the bundled ABI first, then finish small official components
      // early so first-use progress also confirms extraction before large CEF.
      const archives = plan.archives.slice().sort((a: LinuxSteamDownloadArchive, b: LinuxSteamDownloadArchive): number => {
        if (a.url.startsWith('rawfile://')) return -1;
        if (b.url.startsWith('rawfile://')) return 1;
        return a.bytes - b.bytes;
      });
      for (const archive of archives) {
        this.checkPaused(); this.value.component = archive.name;
        const cache = `${root}/downloads/${archive.sha256}.zip`;
        if (archive.url.startsWith('rawfile://')) {
          this.stage('installing', '正在准备应用内置的 GNU 运行组件');
          await this.embedded(context, archive, cache);
        } else {
          this.stage(uri ? 'verifying' : 'downloading', `${uri ? '正在检查' : '正在下载'} ${archive.name}`); this.speedAt = Date.now(); this.speedBase = 0;
          await this.transfer.fetch(archive, cache, (received: number): void => {
            this.value.receivedBytes = completed + received;
            const elapsed = Date.now() - this.speedAt;
            if (elapsed >= 500) { this.value.speedBytes = Math.max(0, (received - this.speedBase) * 1000 / elapsed);
              this.speedAt = Date.now(); this.speedBase = received; }
            this.publish(false);
          }, !uri);
          completed += archive.bytes; this.value.receivedBytes = completed;
        }
        this.stage('verifying', `正在校验 ${archive.name}`); this.checkPaused();
        const unpacked = `${staging}/package`;
        this.step = 'clear-unpack';
        removeInstallationScratch(unpacked); installationDirectory(unpacked);
        this.stage('installing', `正在安装 ${archive.name}`);
        // Archive and selected file SHAs are pinned. Archive paths/links are
        // audited by build_linux_steam_distribution.py before bundling the plan.
        this.step = 'unpack';
        await zlib.decompressFile(cache, unpacked, {}); this.checkPaused();
        for (const file of plan.files) {
          if (file.archive !== archive.id) continue;
          this.checkPaused();
          this.step = `verify-file:${file.path}`;
          let source = `${unpacked}/${file.source}`;
          if (!fs.accessSync(source)) source = `${unpacked}/${file.zipPath}`;
          const stat = fs.lstatSync(source);
          if (!stat.isFile() || stat.isSymbolicLink() || stat.size !== file.bytes ||
            await ConfigurationFiles.fileHash(source) !== file.sha256) {
            throw new LinuxSteamDownloadError('FILE_CHECKSUM', `${archive.name} 中有文件校验失败，请重试`);
          }
          const destination = `${staging}/${file.path}`;
          installationDirectory(destination.substring(0, destination.lastIndexOf('/')));
          this.step = `copy-file:${file.path}`;
          if (fs.accessSync(destination)) {
            if (fs.lstatSync(destination).isSymbolicLink() || !fs.lstatSync(destination).isFile()) throw new Error('组件临时目标无效');
            fs.unlinkSync(destination);
          }
          await fs.copyFile(source, destination);
          this.value.preparedFiles++;
          if (this.value.preparedFiles % 32 === 0) this.publish(false);
        }
        this.step = 'clear-completed-unpack';
        removeInstallationScratch(unpacked); this.value.completedComponents++; this.publish();
      }
      this.checkPaused(); this.value.component = ''; this.stage('installing', '正在启用 Steam 运行组件');
      const release = `${root}/releases/${plan.id}`; installationDirectory(`${root}/releases`);
      if (fs.accessSync(release)) {
        if (ConfigurationFiles.text(ConfigurationFiles.read(`${release}/external-manifest.json`)) !== plan.releaseManifest) {
          throw new LinuxSteamDownloadError('DEPLOYMENT', '同版本组件存在冲突，请保留旧资源并联系开发者');
        }
        // Same-version repair replaces only manifest-listed component files.
        // User data is outside releases/ and never participates in this copy.
        for (const file of plan.files) {
          if (!file.path.startsWith('release/')) continue;
          const destination = `${release}/${file.path.substring(8)}`;
          installationDirectory(destination.substring(0, destination.lastIndexOf('/')));
          const temporary = destination + '.component-update';
          if (fs.accessSync(temporary)) fs.unlinkSync(temporary);
          await fs.copyFile(`${staging}/${file.path}`, temporary);
          if (await ConfigurationFiles.fileHash(temporary) !== file.sha256) throw new Error('运行组件写入校验失败');
          fs.renameSync(temporary, destination);
        }
        // Reset retires this marker while retaining the manifest and unknown
        // files. Restore it only after every runtime file was checked/repaired.
        ConfigurationFiles.json(`${release}/.verified`, plan.id);
      } else {
        ConfigurationFiles.json(`${staging}/release/external-manifest.json`, plan.releaseManifest);
        ConfigurationFiles.json(`${staging}/release/.verified`, plan.id);
        fs.renameSync(`${staging}/release`, release);
      }
      const home = `${root}/client/.steam/steam`; installationDirectory(home);
      for (const file of plan.files) {
        if (!file.path.startsWith('client/')) continue;
        this.checkPaused(); const destination = `${home}/${file.path.substring(7)}`;
        installationDirectory(destination.substring(0, destination.lastIndexOf('/')));
        if (fs.accessSync(destination) && fs.lstatSync(destination).isSymbolicLink()) throw new Error('Steam 资源目标含符号链接');
        const temporary = destination + '.component-update';
        if (fs.accessSync(temporary)) fs.unlinkSync(temporary);
        await fs.copyFile(`${staging}/${file.path}`, temporary);
        if (await ConfigurationFiles.fileHash(temporary) !== file.sha256) {
          throw new LinuxSteamDownloadError('FILE_CHECKSUM', 'Steam 安装文件写入校验失败，请重试');
        }
        fs.renameSync(temporary, destination);
      }
      ConfigurationFiles.json(`${home}/.packaged-data-version`, plan.clientDataId);
      await prepareLinuxSteamClientData(context, false, plan.id);
      removeInstallationScratch(staging);
      for (const archive of plan.archives) {
        for (const suffix of ['.zip', '.zip.part', '.zip.part.append-v1']) {
          try { fs.unlinkSync(`${root}/downloads/${archive.sha256}${suffix}`); } catch (_) {}
        }
      }
      this.stage('ready', 'Steam 安装完成，可以打开并登录');
    } catch (error) {
      const failure = error as LinuxSteamDownloadError;
      this.value.errorCode = String(failure.code || 'INSTALL'); this.value.error = failure.message;
      try { ConfigurationFiles.json(`${BASE}/component-error.json`, JSON.stringify({ step: this.step,
        code: this.value.errorCode, message: failure.message, stack: failure.stack })); } catch (_) {}
      this.stage(this.paused || failure.code === 'PAUSED' ? 'paused' : 'failed',
        this.paused ? '安装已暂停，点击继续即可恢复' : '安装未完成，已下载内容会保留');
      if (this.value.phase === 'failed') throw error as Error;
    }
  }
  async resetComponents(context: common.Context): Promise<void> {
    if (this.operation) throw new Error('请先暂停安装任务，待暂停完成后再重置');
    const task = this.performReset(context); this.operation = task;
    try { await task; } finally { if (this.operation === task) this.operation = null; }
  }
  private async performReset(context: common.Context): Promise<void> {
    try {
      await this.initialize(context);
      const root = await prepareLinuxSteamExternalStorage(context);
      installationDirectory(root);
      const plan = this.plan; if (!plan) throw new Error('组件清单不可用');
      this.stage('installing', '正在重置运行组件，保留游戏库、账号与存档');
      // Invalidate entrypoints before removing any known component files.
      // OHOS TextEncoder.encodeInto('') can return undefined. A newline is an
      // explicit inactive record (all readers trim), without an empty buffer.
      this.step = 'reset-invalidate';
      ConfigurationFiles.json(`${root}/active-version.txt`, '\n');
      if (fs.accessSync(`${BASE}/external-active.path`)) fs.unlinkSync(`${BASE}/external-active.path`);
      this.step = 'reset-runtime';
      await reclaimLinuxSteamReleases(root, '', '', false);
      this.step = 'reset-client-resources';
      for (const file of plan.files) {
        if (!file.path.startsWith('client/')) continue;
        const destination = `${root}/client/.steam/steam/${file.path.substring(7)}`;
        try {
          if (safeInstallationParents(root, destination) && fs.accessSync(destination) && fs.lstatSync(destination).isFile() && !fs.lstatSync(destination).isSymbolicLink() &&
            fs.statSync(destination).size === file.bytes && await ConfigurationFiles.fileHash(destination) === file.sha256) fs.unlinkSync(destination);
        } catch (_) {}
      }
      for (const archive of plan.archives) {
        for (const suffix of ['.zip', '.zip.part', '.zip.part.append-v1']) {
          const file = `${root}/downloads/${archive.sha256}${suffix}`;
          if (safeInstallationParents(root, file) && fs.accessSync(file) && fs.lstatSync(file).isFile() && !fs.lstatSync(file).isSymbolicLink()) fs.unlinkSync(file);
        }
      }
      installationDirectory(`${root}/staging`);
      removeInstallationScratch(`${root}/staging/${plan.id}`);
      this.value.receivedBytes = 0; this.value.completedComponents = 0; this.value.preparedFiles = 0;
      this.value.component = ''; this.value.error = ''; this.value.errorCode = '';
      this.stage('idle', '运行组件已重置，游戏库、账号与存档已保留');
    } catch (error) {
      this.value.error = (error as Error).message; this.value.errorCode = 'RESET';
      try { ConfigurationFiles.json(`${BASE}/component-error.json`, JSON.stringify({ step: this.step,
        code: 'RESET', message: (error as Error).message, stack: (error as Error).stack })); } catch (_) {}
      this.stage('failed', '重置未完成，游戏库未清理'); throw error as Error;
    }
  }
  async ensureReady(context: common.Context): Promise<void> {
    if (!this.installed()) throw new Error('请先下载 Steam 运行组件，下载进度和重试入口可在 Steam 页面查看');
    await prepareLinuxSteamClientData(context, false);
    const root = LinuxSteamLibraryService.getInstance().externalRoot();
    if (!fs.accessSync(`${root}/client/.steam/steam/steamui/index.html`)) throw new Error('Steam 资源不完整，请修复运行组件');
  }
}
