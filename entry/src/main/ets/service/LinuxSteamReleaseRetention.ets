import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface RetainedFile { path: string; sha256: string; bytes: number; }
interface RetainedManifest {
  schemaVersion: number;
  layout: string;
  executionModel: string;
  files: RetainedFile[];
}
interface KnownRelease { version: string; manifest: RetainedManifest; modified: number; complete: boolean; }
export interface SteamReleaseCleanup { releasedBytes: number; removedFiles: number; skippedFiles: number; }

function privateRuntimeParents(release: string, relative: string): boolean {
  const parts = relative.split('/');
  let directory = release;
  for (let index = 0; index < parts.length - 1; index++) {
    directory += `/${parts[index]}`;
    const stat = fs.lstatSync(directory);
    if (!stat.isDirectory() || stat.isSymbolicLink()) return false;
  }
  return true;
}

// Do not recursively remove directories. Only reclaim unchanged, manifest-listed
// runtime cache files; client profiles, saves and unrecognized content are excluded.
export async function reclaimLinuxSteamReleases(root: string, active: string, pending: string,
  keepRollback: boolean): Promise<SteamReleaseCleanup> {
  const result: SteamReleaseCleanup = { releasedBytes: 0, removedFiles: 0, skippedFiles: 0 };
  const directory = `${root}/releases`;
  if (!fs.accessSync(directory)) return result;
  if (!fs.lstatSync(directory).isDirectory() || fs.lstatSync(directory).isSymbolicLink()) return result;
  const known: KnownRelease[] = [];
  for (const version of fs.listFileSync(directory)) {
    if (!/^[a-f0-9]{64}$/.test(version) || version === active || version === pending) continue;
    const release = `${directory}/${version}`;
    try {
      if (!fs.lstatSync(release).isDirectory() || fs.lstatSync(release).isSymbolicLink()) continue;
      const path = `${release}/external-manifest.json`;
      const bytes = ConfigurationFiles.read(path, 1048576);
      if (await ConfigurationFiles.hash(bytes) !== version) continue;
      const manifest = JSON.parse(ConfigurationFiles.text(bytes)) as RetainedManifest;
      if (manifest.schemaVersion !== 1 || manifest.layout !== 'SteamLinuxARM64' ||
          manifest.executionModel !== 'native_aarch64' || !Array.isArray(manifest.files) ||
          manifest.files.length < 1 || manifest.files.length > 512) continue;
      const seen: Set<string> = new Set<string>();
      let valid = true;
      for (const file of manifest.files) {
        if (!/^runtime\/[A-Za-z0-9_.+\/\-]+$/.test(file.path) ||
            file.path.split('/').some((part: string): boolean => part === '' || part === '.' || part === '..') ||
            seen.has(file.path) || !/^[a-f0-9]{64}$/.test(file.sha256) ||
            !Number.isInteger(file.bytes) || file.bytes < 1 || file.bytes > 536870912) { valid = false; break; }
        seen.add(file.path);
      }
      const marker = `${release}/.verified`;
      const complete = fs.accessSync(marker) &&
        ConfigurationFiles.text(ConfigurationFiles.read(marker, 128)).trim() === version;
      if (valid) known.push({ version, manifest, modified: fs.statSync(path).mtime, complete });
    } catch (_) { result.skippedFiles++; }
  }
  known.sort((a: KnownRelease, b: KnownRelease): number => b.modified - a.modified);
  const candidate = known.find((item: KnownRelease): boolean => item.complete);
  const rollback = keepRollback && candidate ? candidate.version : '';
  for (const item of known) {
    if (item.version === rollback) continue;
    const release = `${directory}/${item.version}`;
    for (const file of item.manifest.files) {
      const path = `${release}/${file.path}`;
      try {
        if (!privateRuntimeParents(release, file.path)) { result.skippedFiles++; continue; }
        if (!fs.accessSync(path)) continue;
        const stat = fs.lstatSync(path);
        if (!stat.isFile() || stat.isSymbolicLink() || stat.size !== file.bytes ||
            await ConfigurationFiles.fileHash(path) !== file.sha256) { result.skippedFiles++; continue; }
        // Retire the cache before its first deletion, so incomplete cleanup can
        // never leave an apparently verified runtime behind.
        const marker = `${release}/.verified`;
        if (fs.accessSync(marker)) fs.unlinkSync(marker);
        fs.unlinkSync(path);
        result.releasedBytes += file.bytes; result.removedFiles++;
      } catch (_) { result.skippedFiles++; }
    }
  }
  return result;
}
