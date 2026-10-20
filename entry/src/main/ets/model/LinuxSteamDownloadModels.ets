export type LinuxSteamInstallPhase = 'idle' | 'checking' | 'downloading' | 'verifying' | 'installing' | 'paused' | 'ready' | 'failed';
export interface LinuxSteamInstallSnapshot {
  phase: LinuxSteamInstallPhase;
  message: string;
  component: string;
  receivedBytes: number;
  totalBytes: number;
  speedBytes: number;
  completedComponents: number;
  totalComponents: number;
  preparedFiles: number;
  totalFiles: number;
  requiredSpaceBytes: number;
  errorCode: string;
  error: string;
}
export interface LinuxSteamDownloadArchive {
  id: string; name: string; url: string; bytes: number; sha256: string; unpackedBytes: number;
}
export interface LinuxSteamDownloadFile {
  path: string; archive: string; source: string; zipPath: string; bytes: number; sha256: string;
}
export interface LinuxSteamDeploymentPlan {
  schemaVersion: number; hostAbi: number; id: string; clientBuild: string; clientDataId: string;
  downloadBytes: number; requiredSpaceBytes: number;
  archives: LinuxSteamDownloadArchive[]; files: LinuxSteamDownloadFile[]; releaseManifest: string;
}
export function linuxSteamInstallBusy(phase: LinuxSteamInstallPhase): boolean {
  return phase === 'checking' || phase === 'downloading' || phase === 'verifying' || phase === 'installing';
}
export function formatLinuxSteamBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '0 B';
  if (bytes >= 1073741824) return `${(bytes / 1073741824).toFixed(2)} GB`;
  if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`;
  return `${Math.floor(bytes / 1024)} KB`;
}
export function validLinuxSteamRelativePath(path: string): boolean {
  return path.length > 0 && path.length <= 1024 && !/^[\/]/.test(path) && !/[\\:\x00-\x1f]/.test(path) &&
    path.split('/').every((part: string): boolean => part.length > 0 && part !== '.' && part !== '..');
}
export function validateLinuxSteamDeployment(plan: LinuxSteamDeploymentPlan): void {
  const hash = /^[a-f0-9]{64}$/;
  if (plan.schemaVersion !== 1 || plan.hostAbi !== 1 || !hash.test(plan.id) || !hash.test(plan.clientDataId) ||
    !/^[0-9]{1,20}$/.test(plan.clientBuild) || plan.archives.length < 2 || plan.archives.length > 32 ||
    plan.files.length < 5 || plan.files.length > 20000 || plan.releaseManifest.length > 1048576) {
    throw new Error('组件清单版本不兼容，请更新应用');
  }
  const archives: Set<string> = new Set<string>(); let total = 0;
  for (const item of plan.archives) {
    if (!/^[a-z0-9_-]{1,80}$/.test(item.id) || archives.has(item.id) || !hash.test(item.sha256) ||
      !Number.isSafeInteger(item.bytes) || item.bytes <= 0 || item.bytes > 536870912 ||
      !Number.isSafeInteger(item.unpackedBytes) || item.unpackedBytes < 0 || item.unpackedBytes > 4294967296 ||
      !(/^https:\/\/client-update\.steamstatic\.com\/[A-Za-z0-9_.-]+$/.test(item.url) ||
      (item.id === 'gnu-runtime' && item.url === 'rawfile://linux-steam/hmosdeck-gnu-arm64-v1.zip'))) {
      throw new Error('组件下载来源或大小不正确，请更新应用');
    }
    archives.add(item.id); if (item.url.startsWith('https://')) total += item.bytes;
  }
  const seen: Set<string> = new Set<string>(); let unpacked = 0;
  for (const item of plan.files) {
    if (!validLinuxSteamRelativePath(item.path) || !/^(release\/runtime\/|client\/)/.test(item.path) ||
      !validLinuxSteamRelativePath(item.source) || item.zipPath.replace(/\\/g, '/') !== item.source ||
      !archives.has(item.archive) || seen.has(item.path) || !hash.test(item.sha256) ||
      !Number.isSafeInteger(item.bytes) || item.bytes < 0 || item.bytes > 536870912 ||
      /^client\/(config|userdata|logs|appcache|steamapps)(\/|$)/.test(item.path)) {
      throw new Error('组件文件清单不正确，请更新应用');
    }
    seen.add(item.path); unpacked += item.bytes;
  }
  if (total !== plan.downloadBytes || !Number.isSafeInteger(plan.requiredSpaceBytes) ||
    plan.requiredSpaceBytes < total + unpacked + 536870912 || plan.requiredSpaceBytes > 17179869184 ||
    !seen.has('release/runtime/steamrtarm64/steam') || !seen.has('client/steamui/index.html')) {
    throw new Error('组件清单不完整，请更新应用');
  }
}
