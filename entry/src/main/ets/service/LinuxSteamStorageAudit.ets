import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { readSmokeText } from './ManagedSmokePayloadService';

interface StorageRow { area: string; path: string; depth: number; bytes: number; files: number; directories: number; }
interface StorageBudget { entries: number; errors: number; truncated: boolean; deadline: number; }
interface StorageAudit { schemaVersion: number; rows: StorageRow[]; entries: number; errors: number; truncated: boolean; }

async function measure(area: string, path: string, depth: number, rows: StorageRow[],
  budget: StorageBudget): Promise<StorageRow> {
  const row: StorageRow = { area, path, depth, bytes: 0, files: 0, directories: 0 };
  if (budget.entries >= 150000 || Date.now() > budget.deadline || depth > 30) {
    budget.truncated = true; return row;
  }
  budget.entries++;
  if (budget.entries % 256 === 0) await new Promise<void>((resolve): void => { setTimeout(resolve, 1); });
  try {
    const stat = fs.lstatSync(path);
    if (stat.isSymbolicLink()) return row;
    if (stat.isFile()) { row.bytes = stat.size; row.files = 1; }
    else if (stat.isDirectory()) {
      row.directories = 1;
      for (const name of fs.listFileSync(path)) {
        if (!name || name === '.' || name === '..' || name.includes('/')) continue;
        const child = await measure(area, `${path}/${name}`, depth + 1, rows, budget);
        row.bytes += child.bytes; row.files += child.files; row.directories += child.directories;
        if (budget.truncated) break;
      }
    }
  } catch (_) { budget.errors++; }
  // Directory totals overlap by design: depth=0 is the area's total, deeper
  // rows explain it. No file contents or complete file inventory are collected.
  // Wine prefix and old diagnostic payloads are nested below files/.wine/drive_c.
  // Include that breakdown without collecting filenames below those directories.
  if (depth <= (area === 'app-private' ? 5 : 3)) rows.push(row);
  return row;
}

export async function auditLinuxSteamStorage(context: common.Context): Promise<string> {
  const rows: StorageRow[] = [];
  const budget: StorageBudget = { entries: 0, errors: 0, truncated: false, deadline: Date.now() + 45000 };
  // UIAbility.filesDir is module-scoped (haps/entry/files). Wine and older
  // diagnostics use the application-scoped files/temp directories as well.
  await measure('app-private', '/data/storage/el2/base', 0, rows, budget);
  await measure('installed-code', context.bundleCodeDir, 0, rows, budget);
  const external = readSmokeText('/data/storage/el2/base/files/steamohos-native/external-root.path').trim();
  if (external.endsWith('/SteamLinuxARM64')) await measure('external-steam', external, 0, rows, budget);
  rows.sort((a: StorageRow, b: StorageRow): number => b.bytes - a.bytes);
  const result: StorageAudit = { schemaVersion: 1, rows: rows.slice(0, 100), entries: budget.entries,
    errors: budget.errors, truncated: budget.truncated };
  return JSON.stringify(result);
}
