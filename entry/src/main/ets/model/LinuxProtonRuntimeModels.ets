// Upstream packages and the OHOS adapter have independent identities.
export const PROTON_ADAPTER_ABI = 'hmosdeck-ge-wine11-arm64ec-v1';
export const PROTON_CATALOG_URL = 'https://raw.githubusercontent.com/yifengling0/VintagePomeloPro/main_proton_steamarm64/docs/linux-steam/proton-runtimes/catalog.json';
export interface ProtonRuntimeFile { path: string; bytes: number; sha256: string; mode: number; sourcePath?: string; }
export interface ProtonRuntimeArchive { url: string; bytes: number; sha256: string; unpackedBytes: number; }
export interface ProtonRuntimeRelease {
  id: string; name: string; architecture: string; adapterAbi: string; prefixAbi: string;
  archive: ProtonRuntimeArchive; installedBytes: number; files: ProtonRuntimeFile[];
}
export interface ProtonRuntimeCatalog { schemaVersion: number; revision: number; releases: ProtonRuntimeRelease[]; }
export type ProtonRuntimeSource = 'embedded' | 'download' | 'local' | 'steam';
export interface ProtonRuntimeVersion { id: string; name: string; source: ProtonRuntimeSource; active: boolean; }
export interface ProtonSteamRuntime { path: string; name: string; releaseId: string; supported: boolean; }
export interface ProtonRuntimeSnapshot {
  busy: boolean; message: string; error: string; receivedBytes: number; totalBytes: number;
  activeId: string; previousId: string; versions: ProtonRuntimeVersion[]; available: ProtonRuntimeRelease[];
}
export function validProtonRuntimePath(path: string): boolean {
  return path.length > 8 && path.length <= 512 && path.startsWith('package/') &&
    !/[\\:\x00-\x1f]/.test(path) && path.split('/').every((part: string): boolean => part.length > 0 && part !== '.' && part !== '..');
}
export function validateProtonRuntime(release: ProtonRuntimeRelease): void {
  const hash = /^[a-f0-9]{64}$/;
  if (!release || !hash.test(release.id) || typeof release.name !== 'string' || release.name.length < 1 || release.name.length > 96 ||
      /[\x00-\x1f]/.test(release.name) || release.architecture !== 'aarch64' || release.adapterAbi !== PROTON_ADAPTER_ABI ||
      release.prefixAbi !== 'wine11-arm64ec-v1' || !Array.isArray(release.files) || release.files.length < 20 || release.files.length > 4096) {
    throw new Error('此 Proton 架构或适配版本尚不受支持，请保留现有版本');
  }
  const archive = release.archive;
  if (!archive || !/^https:\/\/github\.com\/yifengling0\/VintagePomeloPro\/releases\/download\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\.zip$/.test(archive.url) ||
      !hash.test(archive.sha256) || !Number.isSafeInteger(archive.bytes) || archive.bytes < 1 || archive.bytes > 536870912 ||
      !Number.isSafeInteger(archive.unpackedBytes) || archive.unpackedBytes < release.installedBytes || archive.unpackedBytes > 1073741824) {
    throw new Error('Proton 下载来源或大小无效');
  }
  const seen = new Set<string>(); let total = 0;
  for (const file of release.files) {
    if (!validProtonRuntimePath(file.path) || (file.sourcePath !== undefined && !validProtonRuntimePath(file.sourcePath)) || seen.has(file.path) || !hash.test(file.sha256) ||
        !Number.isSafeInteger(file.bytes) || file.bytes < 1 || file.bytes > 67108864 || ![384, 448].includes(file.mode)) {
      throw new Error('Proton 运行包文件清单无效');
    }
    seen.add(file.path); total += file.bytes;
  }
  for (const path of ['package/proton', 'package/files/lib/wine/aarch64-unix/wine', 'package/files/bin-arm64/wineserver',
    'package/files/lib/wine/aarch64-unix/ntdll.so', 'package/files/lib/wine/i386-windows/ntdll.dll']) {
    if (!seen.has(path)) throw new Error('Proton 运行包缺少必要文件');
  }
  if (total !== release.installedBytes || total > 1073741824) throw new Error('Proton 运行包展开大小无效');
}
export function validateProtonRuntimeCatalog(catalog: ProtonRuntimeCatalog): void {
  if (!catalog || catalog.schemaVersion !== 1 || !Number.isSafeInteger(catalog.revision) || catalog.revision < 1 ||
      !Array.isArray(catalog.releases) || catalog.releases.length < 1 || catalog.releases.length > 16) throw new Error('Proton 版本列表无效');
  const seen = new Set<string>();
  for (const release of catalog.releases) {
    validateProtonRuntime(release);
    if (seen.has(release.id)) throw new Error('Proton 版本列表包含重复项');
    seen.add(release.id);
  }
}
