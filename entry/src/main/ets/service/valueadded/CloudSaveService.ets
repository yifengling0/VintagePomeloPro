import { common } from '@kit.AbilityKit';
import http from '@ohos.net.http';
import { fileIo as fs } from '@kit.CoreFileKit';
import util from '@ohos.util';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { AppDescriptor } from '../../model/AppModels';
import { SaveManifest, safeRelativeSavePath } from '../../model/ValueAddedModels';
import { CLOUD_CHUNK_BYTES, CloudSaveSlot, CloudSaveIndex, CloudSaveBinding, CloudSaveBegin, CloudSaveVersion,
  CloudSaveDownloadPlan, CloudSaveDownload, CloudSaveManifest, validateCloudSaveManifest, validateCloudSlot } from '../../model/CloudSaveModels';
import { ValueAddedService } from './ValueAddedService';
import { LocalSaveSnapshotService } from './LocalSaveSnapshotService';
interface SaveStatus { status: string; }
interface SlotCreate { label: string; }
interface SaveUploadRequest { slotId: string; baseRevision: number; manifest: CloudSaveManifest; }
interface SaveDeleteRequest { baseRevision: number; }
/** Explicit foreground cloud operations; no automatic scans, uploads, or background authentication. */
export class CloudSaveService {
  private static instance: CloudSaveService | null = null;
  private busy: boolean = false;
  private cancellation: number = 0;
  private localCleanupPending: boolean = false;
  hasLocalCleanupPending(): boolean { return this.localCleanupPending; }
  static getInstance(): CloudSaveService {
    if (!CloudSaveService.instance) CloudSaveService.instance = new CloudSaveService();
    return CloudSaveService.instance;
  }
  private api(): ValueAddedService {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN');
    const api = ValueAddedService.getInstance();
    if (!api.getCapabilities().cloudSaves) throw new Error('NOT_CONFIGURED');
    return api;
  }
  private check(epoch: number, cancellation: number): void {
    this.api().assertCloudEpoch(epoch);
    if (cancellation !== this.cancellation) throw new Error('SAVE_CANCELLED');
  }
  cancel(): void { if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN'); this.cancellation++; }
  async list(): Promise<CloudSaveIndex> {
    const index = await this.api().cloudRequest<CloudSaveIndex>('/v1/saves', http.RequestMethod.GET);
    if (!Array.isArray(index.slots) || index.slots.length > 16 || !Number.isSafeInteger(index.usedBytes) || index.usedBytes < 0 ||
      !Number.isSafeInteger(index.reservedBytes) || index.reservedBytes < 0 || index.quotaBytes !== 1073741824) throw new Error('INVALID_SAVE_SLOT');
    index.slots.forEach(validateCloudSlot); return index;
  }
  async createSlot(label: string): Promise<CloudSaveSlot> {
    const body: SlotCreate = { label: label.trim() };
    const slot = await this.api().cloudRequest<CloudSaveSlot>('/v1/saves/slots', http.RequestMethod.POST, JSON.stringify(body));
    validateCloudSlot(slot); return slot;
  }
  private userKey(api: ValueAddedService): string {
    const id = api.getUserId();
    if (!/^[a-f0-9]{64}$/.test(id)) throw new Error('INVALID_SESSION');
    return id;
  }
  bind(context: common.UIAbilityContext, slot: CloudSaveSlot, appId: string, selectedPath: string): CloudSaveBinding {
    const api = this.api(); api.cloudEpoch(); validateCloudSlot(slot); safeRelativeSavePath(selectedPath);
    if (!LocalSaveSnapshotService.getInstance().eligibleGames().some((app: AppDescriptor): boolean => app.id === appId)) throw new Error('SAVE_GAME_NOT_ELIGIBLE');
    const binding: CloudSaveBinding = { userId: this.userKey(api), slotId: slot.slotId, appId: appId, selectedPath: selectedPath };
    const directory = context.filesDir + '/cloud_save_bindings';
    if (!fs.accessSync(directory)) fs.mkdirSync(directory);
    if (fs.lstatSync(directory).isSymbolicLink()) throw new Error('UNSAFE_SAVE_PATH');
    const target = directory + '/' + binding.userId + '-' + binding.slotId + '.json', temporary = target + '.' + util.generateRandomUUID();
    const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const bytes = new util.TextEncoder().encodeInto(JSON.stringify(binding)); let written = 0;
      while (written < bytes.length) { const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('SAVE_WRITE_FAILED'); written += count; }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
    fs.renameSync(temporary, target); return binding;
  }
  binding(context: common.UIAbilityContext, slot: CloudSaveSlot): CloudSaveBinding | null {
    const api = this.api(); api.cloudEpoch(); validateCloudSlot(slot);
    const file = context.filesDir + '/cloud_save_bindings/' + this.userKey(api) + '-' + slot.slotId + '.json';
    if (!fs.accessSync(file)) return null;
    if (fs.lstatSync(file).isSymbolicLink() || fs.statSync(file).size > 4096) throw new Error('INVALID_SAVE_SLOT');
    const binding = JSON.parse(fs.readTextSync(file)) as CloudSaveBinding;
    if (binding.userId !== api.getUserId() || binding.slotId !== slot.slotId) throw new Error('INVALID_SAVE_SLOT');
    safeRelativeSavePath(binding.selectedPath); return binding;
  }
  async upload(context: common.UIAbilityContext, slot: CloudSaveSlot, snapshot: SaveManifest): Promise<string> {
    const api = this.api(), epoch = api.cloudEpoch(), cancellation = this.cancellation;
    if (this.busy) throw new Error('BUSY'); this.busy = true;
    let jobId = '', transferId = '';
    try {
      const binding = this.binding(context, slot);
      if (!binding || binding.appId !== snapshot.appId || binding.selectedPath !== snapshot.selectedPath) throw new Error('SAVE_BINDING_REQUIRED');
      const local = LocalSaveSnapshotService.getInstance();
      const plan = await local.prepareCloudUpload(context, snapshot.snapshotId, snapshot.appId, (): void => { this.check(epoch, cancellation); });
      transferId = plan.transferId;
      this.check(epoch, cancellation);
      const body: SaveUploadRequest = { slotId: slot.slotId, baseRevision: slot.revision, manifest: plan.manifest };
      const begun = await api.cloudRequest<CloudSaveBegin>('/v1/saves/uploads', http.RequestMethod.POST, JSON.stringify(body));
      if (begun.jobId && /^[a-f0-9-]{36}$/.test(begun.jobId)) jobId = begun.jobId;
      this.check(epoch, cancellation);
      if (begun.status === 'CONFLICT') return 'CONFLICT';
      if (begun.status !== 'READY' || !begun.jobId || !/^[a-f0-9-]{36}$/.test(begun.jobId)) throw new Error('INVALID_SAVE_SLOT');
      jobId = begun.jobId;
      for (const chunk of plan.manifest.chunks) {
        this.check(epoch, cancellation);
        const bytes = await local.readCloudChunk(context, plan.transferId, chunk);
        this.check(epoch, cancellation);
        await api.cloudRequest<SaveStatus>('/v1/saves/uploads/' + jobId + '/chunks/' + chunk.index, http.RequestMethod.PUT, bytes);
      }
      this.check(epoch, cancellation);
      const result = await api.cloudRequest<SaveStatus>('/v1/saves/uploads/' + jobId + '/finalize', http.RequestMethod.POST, '{}');
      this.check(epoch, cancellation);
      if (result.status === 'CONFLICT') return 'CONFLICT';
      if (result.status !== 'PUBLISHED') throw new Error('INVALID_SAVE_SLOT');
      jobId = ''; return 'PUBLISHED';
    } finally {
      if (jobId) { try { api.assertCloudEpoch(epoch);
        await api.cloudRequest<SaveStatus>('/v1/saves/uploads/' + jobId + '/abort', http.RequestMethod.POST, '{}');
      } catch (_) { /* Reservation remains visible; never claim cleanup succeeded on an ambiguous write. */ } }
      this.busy = false;
      if (transferId) { try { LocalSaveSnapshotService.getInstance().cleanupCloudTransfer(context, transferId); }
        catch (_) { this.localCleanupPending = true; } }
    }
  }
  private async fetchChunk(download: CloudSaveDownload, size: number): Promise<ArrayBuffer> {
    // URL comes from the authenticated backend, but is still restricted to official private OBS hosts.
    if (!/^https:\/\/[a-z0-9-]+\.obs\.[a-z0-9-]+\.myhuaweicloud\.com\/[^\s#]+$/.test(download.url) ||
      !Number.isFinite(download.expiresAt) || download.expiresAt <= Date.now() || download.expiresAt > Date.now() + 120000 ||
      size > CLOUD_CHUNK_BYTES) throw new Error('INVALID_OBJECT_URL');
    const client = http.createHttp();
    try {
      const response = await client.request(download.url, { method: http.RequestMethod.GET, header: download.headers,
        expectDataType: http.HttpDataType.ARRAY_BUFFER, maxLimit: CLOUD_CHUNK_BYTES, usingCache: false,
        connectTimeout: 15000, readTimeout: 60000 });
      if (response.responseCode !== 200 || !(response.result instanceof ArrayBuffer) || response.result.byteLength !== size) {
        throw new Error('OBJECT_READ_FAILED');
      }
      return response.result as ArrayBuffer;
    } finally { client.destroy(); }
  }
  async download(context: common.UIAbilityContext, slot: CloudSaveSlot, versionId: string): Promise<SaveManifest> {
    const api = this.api(), epoch = api.cloudEpoch(), cancellation = this.cancellation;
    if (this.busy) throw new Error('BUSY'); this.busy = true;
    let transfer = '';
    try {
      const binding = this.binding(context, slot);
      if (!binding) throw new Error('SAVE_BINDING_REQUIRED');
      if (!slot.versions.some((version: CloudSaveVersion): boolean => version.versionId === versionId)) throw new Error('INVALID_SAVE_SLOT');
      const plan = await api.cloudRequest<CloudSaveDownloadPlan>('/v1/saves/slots/' + slot.slotId + '/versions/' + versionId, http.RequestMethod.GET);
      this.check(epoch, cancellation); validateCloudSaveManifest(plan.manifest);
      if (plan.versionId !== versionId || !Array.isArray(plan.downloads) || plan.downloads.length !== plan.manifest.chunks.length) {
        throw new Error('INVALID_SAVE_MANIFEST');
      }
      const local = LocalSaveSnapshotService.getInstance();
      transfer = local.beginCloudImport(context, plan.manifest, binding.appId, binding.selectedPath);
      for (const chunk of plan.manifest.chunks) {
        this.check(epoch, cancellation);
        const download = await api.cloudRequest<CloudSaveDownload>('/v1/saves/slots/' + slot.slotId + '/versions/' +
          versionId + '/chunks/' + chunk.index, http.RequestMethod.GET);
        this.check(epoch, cancellation);
        if (download.index !== chunk.index) throw new Error('INVALID_SAVE_MANIFEST');
        const bytes = await this.fetchChunk(download, chunk.size);
        this.check(epoch, cancellation);
        await local.writeCloudChunk(context, transfer, chunk, bytes);
      }
      this.check(epoch, cancellation);
      const snapshot = await local.finishCloudImport(context, transfer, (): void => { this.check(epoch, cancellation); });
      this.check(epoch, cancellation); return snapshot;
      // No game files change here. User must explicitly choose restore via the existing local backup/journal path.
    } finally {
      this.busy = false;
      if (transfer) { try { LocalSaveSnapshotService.getInstance().cleanupCloudTransfer(context, transfer); }
        catch (_) { this.localCleanupPending = true; } }
    }
  }
  async deleteVersion(slot: CloudSaveSlot, versionId: string): Promise<void> {
    validateCloudSlot(slot);
    if (!slot.versions.some((version: CloudSaveVersion): boolean => version.versionId === versionId)) throw new Error('INVALID_SAVE_SLOT');
    const body: SaveDeleteRequest = { baseRevision: slot.revision };
    await this.api().cloudRequest<SaveStatus>('/v1/saves/slots/' + slot.slotId + '/versions/' + versionId,
      http.RequestMethod.DELETE, JSON.stringify(body));
  }
  cleanupLocal(context: common.UIAbilityContext): void {
    if (this.busy) throw new Error('BUSY');
    LocalSaveSnapshotService.getInstance().cleanupCloudTransfers(context); this.localCleanupPending = false;
  }
  async cleanup(): Promise<string> { return (await this.api().cloudRequest<SaveStatus>('/v1/saves/cleanup', http.RequestMethod.POST, '{}')).status; }
}
