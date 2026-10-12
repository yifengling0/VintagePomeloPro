import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import util from '@ohos.util';
import testNapi from 'libentry.so';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { AppDescriptor, EngineState, LaunchKind, NativeProcessInfo } from '../../model/AppModels';
import { SaveManifest, SaveManifestEntry, safeRelativeSavePath, validateSaveEntries } from '../../model/ValueAddedModels';
import { CLOUD_CHUNK_BYTES, CloudSaveManifest, CloudUploadPlan, CloudSaveFile, CloudSaveChunk, validateCloudSaveManifest } from '../../model/CloudSaveModels';
import { WineEngineService } from '../WineEngineService';
import { AppCatalogService } from '../AppCatalogService';

interface SteamMarker { appId: number; source: string; name: string; }
interface RestoreJournal { appId: string; selectedPath: string; stagedName: string; previousName: string; originalExists: boolean; phase: string; }
interface CloudImportState { manifest: CloudSaveManifest; appId: string; selectedPath: string; }
interface FileDigest { size: number; modifiedAt: number; sha256: string; }

/** Local-only snapshots. The first version accepts a selected subdirectory of a catalog game's install root. */
export class LocalSaveSnapshotService {
  private static instance: LocalSaveSnapshotService | null = null;
  private busy: boolean = false;
  static getInstance(): LocalSaveSnapshotService {
    if (!LocalSaveSnapshotService.instance) LocalSaveSnapshotService.instance = new LocalSaveSnapshotService();
    return LocalSaveSnapshotService.instance;
  }
  private guard(): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN');
  }
  private ensureStopped(): void {
    // Native ownership can include children launched outside the catalog. Fail closed rather than stopAll.
    if (WineEngineService.getInstance().getState() !== EngineState.STOPPED) throw new Error('SAVE_ENGINE_RUNNING');
    const processes = testNapi.getProcessList() as NativeProcessInfo[];
    if (processes.some((process: NativeProcessInfo): boolean => process.state === 'running')) {
      throw new Error('SAVE_ENGINE_RUNNING');
    }
  }
  private root(context: common.UIAbilityContext): string {
    this.guard();
    const path = context.filesDir + '/local_save_snapshots';
    if (!fs.accessSync(path)) fs.mkdirSync(path);
    return this.canonicalDirectory(path);
  }
  private canonicalDirectory(path: string): string {
    if (fs.lstatSync(path).isSymbolicLink()) throw new Error('UNSAFE_SAVE_PATH');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.DIR | fs.OpenMode.NOFOLLOW);
    try { return file.path; } finally { fs.closeSync(file); }
  }
  private child(root: string, relative: string, directory: boolean = false): string {
    safeRelativeSavePath(relative);
    let path = root;
    const parts = relative.split('/');
    for (let index = 0; index < parts.length; index++) {
      path += '/' + parts[index];
      const stat = fs.lstatSync(path);
      if (stat.isSymbolicLink() || (index < parts.length - 1 && !stat.isDirectory())) throw new Error('UNSAFE_SAVE_PATH');
    }
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW | (directory ? fs.OpenMode.DIR : 0));
    try {
      // CoreFileKit File.path for filesystem paths resolves realpath, not a URI path.
      if (!file.path.startsWith(root + '/')) throw new Error('UNSAFE_SAVE_PATH');
      return file.path;
    } finally { fs.closeSync(file); }
  }
  eligibleGames(): AppDescriptor[] {
    this.guard();
    return AppCatalogService.getInstance().getApps().filter((app: AppDescriptor): boolean =>
      app.available && app.launchTarget.kind === LaunchKind.WINE_EXE && !this.isSteamGame(app));
  }
  private isSteamGame(app: AppDescriptor): boolean {
    try {
      const marker = app.directory + '/.steam-game.json';
      if (!fs.accessSync(marker)) return false;
      // Any marker is conservatively excluded; a verified marker only establishes Steam provenance, not cloud success.
      if (fs.lstatSync(marker).isSymbolicLink() || fs.statSync(marker).size > 8192) return true;
      const parsed = JSON.parse(fs.readTextSync(marker)) as SteamMarker;
      return parsed.source === 'steam' && Number.isInteger(parsed.appId) && parsed.appId > 0 || fs.accessSync(marker);
    } catch (_) { return true; }
  }
  private game(appId: string): AppDescriptor {
    const app = this.eligibleGames().find((candidate: AppDescriptor): boolean => candidate.id === appId);
    if (!app) throw new Error('SAVE_GAME_NOT_ELIGIBLE');
    return app;
  }
  private async digest(path: string, expectedRoot: string, copyTo: string = ''): Promise<FileDigest> {
    const input = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    let output: fs.File | null = null;
    try {
      if (!input.path.startsWith(expectedRoot + '/')) throw new Error('UNSAFE_SAVE_PATH');
      const before = fs.statSync(input.fd);
      if (!before.isFile() || before.size > 67108864) throw new Error('SAVE_QUOTA');
      if (copyTo) output = fs.openSync(copyTo, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
      const md = cryptoFramework.createMd('SHA256');
      const buffer = new ArrayBuffer(65536);
      let total = 0;
      while (true) {
        const count = fs.readSync(input.fd, buffer);
        if (count === 0) break;
        total += count;
        if (total > 67108864) throw new Error('SAVE_QUOTA');
        const bytes = new Uint8Array(buffer.slice(0, count));
        await md.update({ data: bytes });
        if (output) {
          let written = 0;
          while (written < count) {
            const step = fs.writeSync(output.fd, bytes.buffer.slice(written));
            if (step <= 0) throw new Error('SAVE_WRITE_FAILED');
            written += step;
          }
        }
      }
      const after = fs.statSync(input.fd);
      if (before.size !== after.size || before.mtime !== after.mtime || total !== before.size ||
        input.path !== path) throw new Error('SAVE_CHANGED');
      if (output) fs.fsyncSync(output.fd);
      const hash = await md.digest();
      return { size: total, modifiedAt: before.mtime,
        sha256: Array.from(hash.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('') };
    } finally { if (output) fs.closeSync(output); fs.closeSync(input); }
  }
  private async scan(root: string, selected: string, relative: string = '', entries: SaveManifestEntry[] = [],
    visited: string[] = []): Promise<SaveManifestEntry[]> {
    this.ensureStopped();
    const current = relative ? this.child(root, selected + '/' + relative, true) : this.child(root, selected, true);
    visited.push(current);
    if (visited.length > 1024 || relative.split('/').length > 24) throw new Error('SAVE_QUOTA');
    const names = fs.listFileSync(current);
    if (names.length > 2048) throw new Error('SAVE_QUOTA');
    for (const name of names.sort()) {
      const childRelative = relative ? relative + '/' + name : name;
      safeRelativeSavePath(childRelative);
      const path = this.child(root, selected + '/' + childRelative);
      const stat = fs.lstatSync(path);
      if (stat.isDirectory()) await this.scan(root, selected, childRelative, entries, visited);
      else {
        if (!stat.isFile() || entries.length >= 1024) throw new Error('SAVE_QUOTA');
        const digest = await this.digest(path, root);
        entries.push({ relativePath: childRelative, size: digest.size, modifiedAt: digest.modifiedAt, sha256: digest.sha256 });
        validateSaveEntries(entries);
      }
    }
    return entries;
  }
  private writeJson(path: string, value: string): void {
    const temporary = path + '.tmp-' + util.generateRandomUUID();
    const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const bytes = new util.TextEncoder().encodeInto(value);
      let written = 0;
      while (written < bytes.length) {
        const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('SAVE_WRITE_FAILED');
        written += count;
      }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
    fs.renameSync(temporary, path);
  }
  private ensureParents(root: string, relative: string): void {
    const parts = safeRelativeSavePath(relative).split('/');
    parts.pop(); let path = root;
    for (const part of parts) {
      path += '/' + part;
      if (!fs.accessSync(path)) fs.mkdirSync(path);
      if (fs.lstatSync(path).isSymbolicLink() || !fs.statSync(path).isDirectory()) throw new Error('UNSAFE_SAVE_PATH');
    }
  }
  private async snapshotInternal(context: common.UIAbilityContext, appId: string, selectedPath: string, allowEmpty: boolean = false): Promise<SaveManifest> {
    this.ensureStopped(); safeRelativeSavePath(selectedPath);
    const app = this.game(appId), approvedRoot = this.canonicalDirectory(app.directory);
    this.child(approvedRoot, selectedPath, true);
    const first = await this.scan(approvedRoot, selectedPath);
    validateSaveEntries(first, allowEmpty);
    await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 350); });
    const second = await this.scan(approvedRoot, selectedPath);
    if (JSON.stringify(first) !== JSON.stringify(second)) throw new Error('SAVE_CHANGED');
    const id = util.generateRandomUUID();
    const base = this.root(context), staging = base + '/pending-' + id;
    fs.mkdirSync(staging); fs.mkdirSync(staging + '/files');
    for (const entry of second) {
      this.ensureStopped();
      this.ensureParents(staging + '/files', entry.relativePath);
      const source = this.child(approvedRoot, selectedPath + '/' + entry.relativePath);
      const copied = await this.digest(source, approvedRoot, staging + '/files/' + entry.relativePath);
      if (copied.sha256 !== entry.sha256 || copied.size !== entry.size || copied.modifiedAt !== entry.modifiedAt) {
        throw new Error('SAVE_CHANGED');
      }
    }
    const finalScan = await this.scan(approvedRoot, selectedPath);
    if (JSON.stringify(finalScan) !== JSON.stringify(second)) throw new Error('SAVE_CHANGED');
    const manifest: SaveManifest = { schemaVersion: 1, snapshotId: id, appId: app.id,
      rootAlias: 'install', selectedPath: selectedPath, createdAt: Date.now(), files: second };
    this.ensureStopped();
    this.writeJson(staging + '/manifest.json', JSON.stringify(manifest));
    fs.renameSync(staging, base + '/' + id);
    return manifest;
  }
  async create(context: common.UIAbilityContext, appId: string, selectedPath: string): Promise<SaveManifest> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try { return await this.snapshotInternal(context, appId, selectedPath); } finally { this.busy = false; }
  }
  list(context: common.UIAbilityContext, appId: string): SaveManifest[] {
    this.guard();
    const base = this.root(context), result: SaveManifest[] = [];
    for (const id of fs.listFileSync(base)) {
      if (!/^[a-f0-9-]{36}$/.test(id)) continue;
      try {
        const path = this.child(base, id + '/manifest.json');
        if (fs.statSync(path).size > 524288) continue;
        const manifest = JSON.parse(fs.readTextSync(path)) as SaveManifest;
        if (manifest.appId === appId && manifest.snapshotId === id && manifest.schemaVersion === 1 && manifest.rootAlias === 'install') {
          safeRelativeSavePath(manifest.selectedPath); validateSaveEntries(manifest.files, true); result.push(manifest);
        }
      } catch (_) { }
    }
    return result.sort((a: SaveManifest, b: SaveManifest): number => b.createdAt - a.createdAt);
  }
  recoverInterrupted(context: common.UIAbilityContext): void {
    this.guard(); this.ensureStopped();
    if (this.busy) throw new Error('BUSY');
    const journalPath = this.root(context) + '/restore-journal.json';
    if (!fs.accessSync(journalPath)) return;
    if (fs.lstatSync(journalPath).isSymbolicLink() || fs.statSync(journalPath).size > 4096) throw new Error('SAVE_RECOVERY_REQUIRED');
    const journal = JSON.parse(fs.readTextSync(journalPath)) as RestoreJournal;
    safeRelativeSavePath(journal.selectedPath);
    if (!/^\.vp-restore-[a-f0-9-]{36}$/.test(journal.stagedName) ||
      !/^\.vp-before-[a-f0-9-]{36}$/.test(journal.previousName)) throw new Error('SAVE_RECOVERY_REQUIRED');
    const root = this.canonicalDirectory(this.game(journal.appId).directory);
    const split = journal.selectedPath.lastIndexOf('/');
    const parent = split < 0 ? root : this.child(root, journal.selectedPath.slice(0, split), true);
    const selected = root + '/' + journal.selectedPath;
    const previous = parent + '/' + journal.previousName;
    if (!fs.accessSync(selected)) {
      if (fs.accessSync(previous)) {
        if (fs.lstatSync(previous).isSymbolicLink()) throw new Error('SAVE_RECOVERY_REQUIRED');
        this.ensureStopped(); fs.renameSync(previous, selected);
      } else if (journal.originalExists !== false) throw new Error('SAVE_RECOVERY_REQUIRED');
    } else {
      this.child(root, journal.selectedPath, true);
    }
    // If both directories exist, a swap had completed. Keep both; no user data is deleted.
    fs.unlinkSync(journalPath);
  }
  private cloudDirectory(context: common.UIAbilityContext, id: string): string {
    if (!/^[a-f0-9-]{36}$/.test(id)) throw new Error('INVALID_SAVE_MANIFEST');
    return this.child(this.root(context), 'transfer-' + id, true);
  }
  private writeBytes(path: string, bytes: Uint8Array): void {
    const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW | fs.OpenMode.TRUNC);
    try {
      let written = 0;
      while (written < bytes.length) {
        const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('SAVE_WRITE_FAILED');
        written += count;
      }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
  }
  private async bytesHash(bytes: Uint8Array): Promise<string> {
    const md = cryptoFramework.createMd('SHA256');
    await md.update({ data: bytes });
    const result = await md.digest();
    return Array.from(result.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
  }
  /** Pack concatenated file bytes into bounded blocks; no archive extraction or device paths reach the cloud. */
  async prepareCloudUpload(context: common.UIAbilityContext, snapshotId: string, appId: string, assertCurrent: () => void): Promise<CloudUploadPlan> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    const id = util.generateRandomUUID();
    try {
      assertCurrent();
      const snapshot = this.list(context, appId).find((item: SaveManifest): boolean => item.snapshotId === snapshotId);
      if (!snapshot) throw new Error('INVALID_SAVE_MANIFEST');
      const directory = this.root(context) + '/transfer-' + id;
      fs.mkdirSync(directory);
      const sourceRoot = this.canonicalDirectory(this.root(context) + '/' + snapshotId + '/files');
      const files: CloudSaveFile[] = [], chunks: CloudSaveChunk[] = [];
      let offset = 0, used = 0;
      let packed = new Uint8Array(CLOUD_CHUNK_BYTES);
      for (const entry of snapshot.files) {
        assertCurrent();
        const source = this.child(sourceRoot, entry.relativePath), file = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
        const md = cryptoFramework.createMd('SHA256');
        let size = 0;
        try {
          if (!file.path.startsWith(sourceRoot + '/')) throw new Error('UNSAFE_SAVE_PATH');
          while (true) {
            const buffer = new ArrayBuffer(65536), count = fs.readSync(file.fd, buffer);
            if (count === 0) break;
            size += count;
            if (size > entry.size) throw new Error('SAVE_CHANGED');
            const bytes = new Uint8Array(buffer.slice(0, count));
            await md.update({ data: bytes });
            assertCurrent();
            let read = 0;
            while (read < bytes.length) {
              const step = Math.min(CLOUD_CHUNK_BYTES - used, bytes.length - read);
              packed.set(bytes.slice(read, read + step), used); used += step; read += step;
              if (used === CLOUD_CHUNK_BYTES) {
                const hash = await this.bytesHash(packed);
                assertCurrent();
                this.writeBytes(directory + '/' + chunks.length, packed);
                chunks.push({ index: chunks.length, size: used, sha256: hash });
                packed = new Uint8Array(CLOUD_CHUNK_BYTES); used = 0;
              }
            }
          }
        } finally { fs.closeSync(file); }
        const digest = await md.digest();
        const hash = Array.from(digest.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
        if (size !== entry.size || hash !== entry.sha256) throw new Error('SAVE_HASH_MISMATCH');
        files.push({ relativePath: entry.relativePath, size: size, sha256: hash, offset: offset }); offset += size;
      }
      if (used > 0) {
        const tail = packed.slice(0, used), hash = await this.bytesHash(tail);
        this.writeBytes(directory + '/' + chunks.length, tail);
        chunks.push({ index: chunks.length, size: used, sha256: hash });
      }
      const manifest: CloudSaveManifest = { schemaVersion: 1, files: files, chunks: chunks, totalBytes: offset };
      validateCloudSaveManifest(manifest);
      assertCurrent();
      this.writeJson(directory + '/cloud.json', JSON.stringify(manifest));
      return { transferId: id, manifest: manifest };
    } catch (error) {
      try { this.cleanupCloudTransfer(context, id); } catch (_) { }
      throw new Error((error as Error).message);
    } finally { this.busy = false; }
  }
  async readCloudChunk(context: common.UIAbilityContext, transferId: string, chunk: CloudSaveChunk): Promise<ArrayBuffer> {
    this.guard();
    if (!Number.isInteger(chunk.index) || chunk.index < 0 || chunk.index >= 64 || chunk.size < 1 || chunk.size > CLOUD_CHUNK_BYTES) {
      throw new Error('INVALID_SAVE_MANIFEST');
    }
    const directory = this.cloudDirectory(context, transferId), path = this.child(directory, '' + chunk.index);
    if (fs.statSync(path).size !== chunk.size) throw new Error('SAVE_HASH_MISMATCH');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    const bytes = new Uint8Array(chunk.size); let read = 0;
    try {
      while (read < bytes.length) {
        const buffer = new ArrayBuffer(Math.min(65536, bytes.length - read)), count = fs.readSync(file.fd, buffer);
        if (count <= 0) throw new Error('SAVE_HASH_MISMATCH');
        bytes.set(new Uint8Array(buffer.slice(0, count)), read); read += count;
      }
    } finally { fs.closeSync(file); }
    if (await this.bytesHash(bytes) !== chunk.sha256) throw new Error('SAVE_HASH_MISMATCH');
    return bytes.buffer;
  }
  beginCloudImport(context: common.UIAbilityContext, manifest: CloudSaveManifest, appId: string, selectedPath: string): string {
    this.guard(); this.game(appId); safeRelativeSavePath(selectedPath); validateCloudSaveManifest(manifest);
    const id = util.generateRandomUUID(), directory = this.root(context) + '/transfer-' + id;
    fs.mkdirSync(directory);
    const state: CloudImportState = { manifest: manifest, appId: appId, selectedPath: selectedPath };
    try { this.writeJson(directory + '/import.json', JSON.stringify(state)); return id; }
    catch (error) {
      try { this.cleanupCloudTransfer(context, id); } catch (_) { }
      throw new Error((error as Error).message);
    }
  }
  async writeCloudChunk(context: common.UIAbilityContext, transferId: string, chunk: CloudSaveChunk, buffer: ArrayBuffer): Promise<void> {
    this.guard();
    if (buffer.byteLength !== chunk.size || chunk.size > CLOUD_CHUNK_BYTES || !Number.isInteger(chunk.index) || chunk.index < 0 || chunk.index >= 64 ||
      await this.bytesHash(new Uint8Array(buffer)) !== chunk.sha256) throw new Error('SAVE_HASH_MISMATCH');
    this.writeBytes(this.cloudDirectory(context, transferId) + '/' + chunk.index, new Uint8Array(buffer));
  }
  async finishCloudImport(context: common.UIAbilityContext, transferId: string, assertCurrent: () => void): Promise<SaveManifest> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const directory = this.cloudDirectory(context, transferId);
      const statePath = this.child(directory, 'import.json');
      if (fs.statSync(statePath).size > 524288) throw new Error('INVALID_SAVE_MANIFEST');
      const state = JSON.parse(fs.readTextSync(statePath)) as CloudImportState;
      validateCloudSaveManifest(state.manifest); this.game(state.appId); safeRelativeSavePath(state.selectedPath);
      fs.mkdirSync(directory + '/files');
      let chunkIndex = 0, chunkOffset = 0, packed = new Uint8Array(0);
      const entries: SaveManifestEntry[] = [];
      for (const entry of state.manifest.files) {
        assertCurrent();
        this.ensureParents(directory + '/files', entry.relativePath);
        const target = directory + '/files/' + entry.relativePath;
        const file = fs.openSync(target, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
        const md = cryptoFramework.createMd('SHA256'); let written = 0;
        try {
          while (written < entry.size) {
            if (chunkOffset === packed.length) {
              packed = new Uint8Array(await this.readCloudChunk(context, transferId, state.manifest.chunks[chunkIndex++]));
              assertCurrent(); chunkOffset = 0;
            }
            const step = Math.min(entry.size - written, packed.length - chunkOffset);
            const bytes = packed.slice(chunkOffset, chunkOffset + step);
            await md.update({ data: bytes });
            assertCurrent();
            let position = 0;
            while (position < bytes.length) {
              const count = fs.writeSync(file.fd, bytes.buffer.slice(position));
              if (count <= 0) throw new Error('SAVE_WRITE_FAILED'); position += count;
            }
            written += step; chunkOffset += step;
          }
          fs.fsyncSync(file.fd);
        } finally { fs.closeSync(file); }
        const digest = await md.digest();
        const hash = Array.from(digest.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
        if (hash !== entry.sha256) throw new Error('SAVE_HASH_MISMATCH');
        entries.push({ relativePath: entry.relativePath, size: entry.size, sha256: hash, modifiedAt: Date.now() });
      }
      const id = util.generateRandomUUID();
      const manifest: SaveManifest = { schemaVersion: 1, snapshotId: id, appId: state.appId, rootAlias: 'install',
        selectedPath: state.selectedPath, createdAt: Date.now(), files: entries };
      assertCurrent();
      for (const chunk of state.manifest.chunks) fs.unlinkSync(this.child(directory, '' + chunk.index));
      fs.unlinkSync(this.child(directory, 'import.json'));
      this.writeJson(directory + '/manifest.json', JSON.stringify(manifest));
      fs.renameSync(directory, this.root(context) + '/' + id); return manifest;
    } finally { this.busy = false; }
  }
  cleanupCloudTransfer(context: common.UIAbilityContext, transferId: string): void {
    this.guard();
    if (!/^[a-f0-9-]{36}$/.test(transferId)) throw new Error('INVALID_SAVE_MANIFEST');
    const path = this.root(context) + '/transfer-' + transferId;
    if (!fs.accessSync(path)) return;
    const directory = this.cloudDirectory(context, transferId);
    this.removeTransferTree(directory);
  }
  cleanupCloudTransfers(context: common.UIAbilityContext): void {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    for (const name of fs.listFileSync(this.root(context))) {
      if (/^transfer-[a-f0-9-]{36}$/.test(name)) this.cleanupCloudTransfer(context, name.slice(9));
    }
  }
  private removeTransferTree(directory: string): void {
    for (const name of fs.listFileSync(directory)) {
      safeRelativeSavePath(name);
      const path = directory + '/' + name, stat = fs.lstatSync(path);
      if (stat.isDirectory() && !stat.isSymbolicLink()) this.removeTransferTree(path);
      else fs.unlinkSync(path);
    }
    fs.rmdirSync(directory);
  }
  async restore(context: common.UIAbilityContext, snapshotId: string, appId: string): Promise<void> {
    this.guard(); this.ensureStopped();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const manifest = this.list(context, appId).find((item: SaveManifest): boolean => item.snapshotId === snapshotId);
      if (!manifest) throw new Error('INVALID_SAVE_MANIFEST');
      const approvedRoot = this.canonicalDirectory(this.game(appId).directory);
      this.ensureParents(approvedRoot, manifest.selectedPath);
      const originalExists = fs.accessSync(approvedRoot + '/' + manifest.selectedPath);
      const selected = originalExists ? this.child(approvedRoot, manifest.selectedPath, true) : approvedRoot + '/' + manifest.selectedPath;
      // An unresolved interrupted restore is never overwritten. The intact previous directory stays available.
      const journalPath = this.root(context) + '/restore-journal.json';
      if (fs.accessSync(journalPath)) throw new Error('SAVE_RECOVERY_REQUIRED');
      const beforeRestore = originalExists ? await this.snapshotInternal(context, appId, manifest.selectedPath, true) : null;
      const parent = selected.slice(0, selected.lastIndexOf('/'));
      const id = util.generateRandomUUID();
      const stagedName = '.vp-restore-' + id, previousName = '.vp-before-' + id;
      const staged = parent + '/' + stagedName, previous = parent + '/' + previousName;
      fs.mkdirSync(staged);
      const sourceRoot = this.canonicalDirectory(this.root(context) + '/' + snapshotId + '/files');
      for (const entry of manifest.files) {
        this.ensureStopped(); this.ensureParents(staged, entry.relativePath);
        const source = this.child(sourceRoot, entry.relativePath);
        const copied = await this.digest(source, sourceRoot, staged + '/' + entry.relativePath);
        if (copied.sha256 !== entry.sha256 || copied.size !== entry.size) throw new Error('SAVE_HASH_MISMATCH');
      }
      this.ensureStopped();
      if (beforeRestore) {
        const currentFiles = await this.scan(approvedRoot, manifest.selectedPath);
        if (JSON.stringify(currentFiles) !== JSON.stringify(beforeRestore.files)) throw new Error('SAVE_CHANGED');
      } else if (fs.accessSync(selected)) throw new Error('SAVE_CHANGED');
      const journal: RestoreJournal = { appId: appId, selectedPath: manifest.selectedPath, stagedName: stagedName,
        previousName: previousName, originalExists: originalExists, phase: 'prepared' };
      this.writeJson(journalPath, JSON.stringify(journal));
      this.ensureStopped();
      if (originalExists) fs.renameSync(selected, previous);
      try {
        this.ensureStopped(); fs.renameSync(staged, selected);
      } catch (error) {
        if (originalExists) fs.renameSync(previous, selected);
        fs.unlinkSync(journalPath);
        throw new Error((error as Error).message);
      }
      // Retain the pre-restore tree as well as the snapshot. Never delete game data to finish this operation.
      fs.unlinkSync(journalPath);
    } finally { this.busy = false; }
  }
}
