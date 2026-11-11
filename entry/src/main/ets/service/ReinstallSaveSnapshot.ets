// Explicit local maintenance operation only; never restored by startup.
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { linuxSteamDownloadHome } from './LinuxSteamDiagnosticData';
import { ensureSmokeDirectory, writeSmokeText } from './ManagedSmokePayloadService';
import { hilog } from './LogService';

interface SnapshotTotals { files: number; bytes: number; links: number; deadline: number; }

function verifyFile(source: string, target: string): void {
  if (fs.statSync(source).size !== fs.statSync(target).size) throw new Error('Snapshot size mismatch');
  const a = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  try {
    const b = fs.openSync(target, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const left = new ArrayBuffer(65536), right = new ArrayBuffer(65536);
      while (true) {
        const count = fs.readSync(a.fd, left);
        let offset = 0;
        while (offset < count) {
          const part = new ArrayBuffer(count - offset);
          const n = fs.readSync(b.fd, part);
          if (n <= 0) throw new Error('Snapshot truncated');
          new Uint8Array(right).set(new Uint8Array(part, 0, n), offset); offset += n;
        }
        for (let i = 0; i < count; i++) if (new Uint8Array(left)[i] !== new Uint8Array(right)[i]) throw new Error('Snapshot content mismatch');
        if (count === 0) break;
      }
    } finally { fs.closeSync(b); }
  } finally { fs.closeSync(a); }
}

async function copyTree(source: string, target: string, totals: SnapshotTotals, depth: number = 0): Promise<void> {
  if (depth > 40 || totals.files >= 100000 || Date.now() > totals.deadline) throw new Error('Snapshot budget exceeded');
  const stat = fs.lstatSync(source);
  if (stat.isSymbolicLink()) { totals.links++; return; } // Linked public directories remain in the preserved Download tree.
  if (stat.isDirectory()) {
    if (!ensureSmokeDirectory(target)) throw new Error('Cannot create snapshot directory');
    for (const name of fs.listFileSync(source)) {
      if (!name || name === '.' || name === '..' || name.includes('/')) throw new Error('Invalid snapshot child');
      await copyTree(source + '/' + name, target + '/' + name, totals, depth + 1);
    }
  } else if (stat.isFile()) {
    if (totals.bytes + stat.size > 2 * 1024 * 1024 * 1024) throw new Error('Private save snapshot exceeds 2 GiB');
    fs.copyFileSync(source, target); verifyFile(source, target);
    totals.files++; totals.bytes += stat.size;
    if (totals.files % 16 === 0) await new Promise<void>((resolve): void => { setTimeout(resolve, 1); });
  } else throw new Error('Unsupported snapshot entry');
}

export async function preserveReinstallSaves(context: common.Context): Promise<void> {
  const destination = (await linuxSteamDownloadHome(context)) + '/ReinstallPreserved';
  if (!ensureSmokeDirectory(destination)) throw new Error('Cannot create save snapshot');
  const marker = destination + '/native-verified.json';
  try { fs.unlinkSync(marker); } catch (_) {}
  const totals: SnapshotTotals = { files: 0, bytes: 0, links: 0, deadline: Date.now() + 300000 };
  const prefix = '/data/storage/el2/base/files/.wine';
  for (const name of fs.listFileSync(prefix + '/drive_c')) {
    // Preserve every non-system C: directory, including unknown local games.
    if (['windows', 'smoke', 'SteamLinuxARM64'].includes(name)) continue;
    await copyTree(prefix + '/drive_c/' + name, destination + '/drive_c/' + name, totals);
  }
  for (const name of fs.listFileSync(prefix)) {
    if (name.endsWith('.reg')) await copyTree(prefix + '/' + name, destination + '/' + name, totals);
  }
  writeSmokeText(marker, JSON.stringify({ schemaVersion: 1, verified: true,
    files: totals.files, bytes: totals.bytes, skippedPublicLinks: totals.links, automaticRestore: false }));
  hilog.warn(0, 'ReinstallSaveSnapshot', 'SAVE_SNAPSHOT_PASS files=%{public}d bytes=%{public}d links=%{public}d', totals.files, totals.bytes, totals.links);
}
