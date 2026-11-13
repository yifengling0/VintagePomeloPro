import { safeRelativeSavePath } from './ValueAddedModels';
export const CLOUD_CHUNK_BYTES: number = 4194304;
export interface CloudSaveFile { relativePath: string; size: number; sha256: string; offset: number; }
export interface CloudSaveChunk { index: number; size: number; sha256: string; }
export interface CloudSaveManifest { schemaVersion: number; totalBytes: number; files: CloudSaveFile[]; chunks: CloudSaveChunk[]; }
export interface CloudSaveVersion { versionId: string; createdAt: number; totalBytes: number; }
export interface CloudSaveSlot { slotId: string; label: string; revision: number; versions: CloudSaveVersion[]; }
export interface CloudSaveIndex { slots: CloudSaveSlot[]; usedBytes: number; reservedBytes: number; quotaBytes: number; }
export interface CloudUploadPlan { transferId: string; manifest: CloudSaveManifest; }
export interface CloudSaveBegin { status: string; jobId?: string; expiresAt?: number; slot?: CloudSaveSlot; }
export interface CloudSaveDownload { index: number; url: string; headers: Record<string, string>; expiresAt: number; }
export interface CloudSaveDownloadPlan { versionId: string; manifest: CloudSaveManifest; downloads: CloudSaveDownload[]; }
export interface CloudSaveBinding { userId: string; slotId: string; appId: string; selectedPath: string; }
function exact(value: Object, keys: string[]): void {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
    Object.keys(value).some((key: string): boolean => !keys.includes(key))) throw new Error('INVALID_SAVE_MANIFEST');
}
export function validateCloudSaveManifest(value: CloudSaveManifest): void {
  exact(value, ['schemaVersion', 'files', 'chunks', 'totalBytes']);
  if (value.schemaVersion !== 1 || !Array.isArray(value.files) || value.files.length < 1 || value.files.length > 1024 ||
    !Array.isArray(value.chunks) || value.chunks.length > 64 || !Number.isSafeInteger(value.totalBytes) ||
    value.totalBytes < 0 || value.totalBytes > 268435456 || JSON.stringify(value).length > 400000) throw new Error('INVALID_SAVE_MANIFEST');
  let offset = 0;
  const paths: string[] = [];
  value.files.forEach((file: CloudSaveFile): void => {
    exact(file, ['relativePath', 'size', 'sha256', 'offset']);
    const path = safeRelativeSavePath(file.relativePath);
    if (path.length > 240 || path !== path.normalize('NFC') || path.split('/').length > 24) throw new Error('UNSAFE_SAVE_PATH');
    const key = path.toLowerCase();
    if (paths.some((other: string): boolean => other === key || other.startsWith(key + '/') || key.startsWith(other + '/'))) {
      throw new Error('SAVE_PATH_COLLISION');
    }
    paths.push(key);
    if (!Number.isSafeInteger(file.size) || file.size < 0 || file.size > 67108864 || file.offset !== offset ||
      !/^[a-f0-9]{64}$/.test(file.sha256)) throw new Error('INVALID_SAVE_MANIFEST');
    offset += file.size;
  });
  if (offset !== value.totalBytes || value.chunks.length !== Math.ceil(offset / CLOUD_CHUNK_BYTES)) throw new Error('INVALID_SAVE_MANIFEST');
  value.chunks.forEach((chunk: CloudSaveChunk, index: number): void => {
    exact(chunk, ['index', 'size', 'sha256']);
    if (chunk.index !== index || chunk.size !== Math.min(CLOUD_CHUNK_BYTES, offset - index * CLOUD_CHUNK_BYTES) ||
      !/^[a-f0-9]{64}$/.test(chunk.sha256)) throw new Error('INVALID_SAVE_MANIFEST');
  });
}
export function validateCloudSlot(slot: CloudSaveSlot): void {
  if (!slot || !/^[a-f0-9-]{36}$/.test(slot.slotId) || typeof slot.label !== 'string' || slot.label.length > 80 ||
    !Number.isSafeInteger(slot.revision) || slot.revision < 0 || !Array.isArray(slot.versions) || slot.versions.length > 3) {
    throw new Error('INVALID_SAVE_SLOT');
  }
  for (const version of slot.versions) {
    if (!/^[a-f0-9-]{36}$/.test(version.versionId) || !Number.isSafeInteger(version.totalBytes) || version.totalBytes < 0 ||
      version.totalBytes > 268435456 || !Number.isFinite(version.createdAt)) throw new Error('INVALID_SAVE_SLOT');
  }
}
