import { fileIo as fs } from '@kit.CoreFileKit';
import { SteamVdfNode, parseSteamVdf, serializeSteamVdf } from '../steam/SteamLocalRules';
import { ConfigurationFiles } from './ConfigurationFiles';
import { ensureSmokeDirectory, writeSmokeText } from './ManagedSmokePayloadService';

interface LibraryMove { source: string; destination: string; }
export interface LinuxSteamLibraryMigration {
  schemaVersion: number;
  visibleLibraries: number;
  movedEntries: number;
  installedApps: string[];
}

// Config verification must preserve trailing whitespace. readSmokeText trims
// diagnostic values and cannot be used for byte-for-byte VDF write checks.
function libraryText(path: string): string {
  try { return fs.readTextSync(path, { encoding: 'utf-8' }); } catch (_) { return ''; }
}

function controlFile(name: string): boolean {
  return name === 'libraryfolders.vdf' || name.startsWith('libraryfolders.vdf.') ||
    name.startsWith('.hmosdeck-');
}

// Compare only collisions. A whole, non-colliding game directory is renamed,
// never copied or re-downloaded. Do not follow links outside this native library.
async function planMerge(source: string, destination: string, moves: LibraryMove[],
  budget: number[], depth: number = 0): Promise<void> {
  if (depth > 64) throw new Error('Steam 游戏库目录过深，未迁移');
  for (const name of fs.listFileSync(source)) {
    if (++budget[0] > 100000 || name === '.' || name === '..' || name.includes('/')) {
      throw new Error('Steam 游戏库目录异常，未迁移');
    }
    if (depth === 0 && controlFile(name)) continue;
    const from = `${source}/${name}`;
    const to = `${destination}/${name}`;
    const before = fs.lstatSync(from);
    if (before.isSymbolicLink()) throw new Error('Steam 游戏库包含链接，未迁移');
    if (!fs.accessSync(to)) { moves.push({ source: from, destination: to }); continue; }
    const after = fs.lstatSync(to);
    if (after.isSymbolicLink()) throw new Error('Steam 游戏库目标包含链接，未迁移');
    if (before.isDirectory() && after.isDirectory()) {
      await planMerge(from, to, moves, budget, depth + 1);
    } else if (!(before.isFile() && after.isFile() && before.size === after.size &&
      await ConfigurationFiles.fileHash(from) === await ConfigurationFiles.fileHash(to))) {
      throw new Error(`Steam 游戏库存在不同的同名文件，请保留两处数据后处理：${name}`);
    }
  }
}

function atomicText(path: string, text: string): void {
  if (libraryText(path) === text) return;
  const temporary = `${path}.hmosdeck-write-v2`;
  writeSmokeText(temporary, text);
  if (libraryText(temporary) !== text) throw new Error('无法写入 Steam 游戏库配置');
  fs.renameSync(temporary, path);
  if (libraryText(path) !== text) throw new Error('无法确认 Steam 游戏库配置');
}

function preserve(path: string, backup: string): void {
  const text = libraryText(path);
  if (!text || fs.accessSync(backup)) return;
  atomicText(backup, text);
}

function mergedFolders(texts: string[], steam: string, library: string, apps: string[]): string {
  let primary = new SteamVdfNode();
  const combined = new SteamVdfNode();
  for (const text of texts) {
    if (!text.trim()) continue;
    const folders = parseSteamVdf(text).child('libraryfolders');
    if (!folders) throw new Error('Steam 游戏库配置无效，未迁移');
    folders.children.forEach((node: SteamVdfNode, key: string): void => {
      if (!/^[0-9]+$/.test(key)) return;
      const path = node.text('path').replace(/\/+$/, '');
      if (path !== steam && path !== library) throw new Error('发现额外 Steam 游戏库，未自动合并');
      if (path === steam) primary = node;
      node.child('apps')?.children.forEach((app: SteamVdfNode, id: string): void => {
        if (/^[1-9][0-9]{0,9}$/.test(id)) combined.children.set(id, app);
      });
    });
  }
  for (const id of apps) {
    if (!combined.children.has(id)) combined.set(id, '0');
  }
  primary.set('path', steam);
  primary.set('label', 'HmosDeck 游戏');
  primary.children.delete('apps');
  // The shared serializer has no empty-object node. Steam can populate a
  // missing apps map; emitting an empty scalar here would corrupt its type.
  if (combined.children.size > 0) primary.children.set('apps', combined);
  const folders = new SteamVdfNode();
  folders.children.set('0', primary);
  const result = new SteamVdfNode();
  result.children.set('libraryfolders', folders);
  return serializeSteamVdf(result);
}

export async function prepareSingleLinuxSteamLibrary(root: string): Promise<LinuxSteamLibraryMigration> {
  const steam = `${root}/client/.steam/steam`;
  const library = `${root}/library`;
  const source = `${steam}/steamapps`;
  const destination = `${library}/steamapps`;
  for (const directory of [source, destination]) {
    if (!ensureSmokeDirectory(directory)) throw new Error('无法准备 Steam 游戏库');
    if (!fs.lstatSync(directory).isDirectory() || fs.lstatSync(directory).isSymbolicLink()) {
      throw new Error('Steam 游戏库路径异常');
    }
  }
  const apps: string[] = [];
  for (const directory of [source, destination]) {
    for (const name of fs.listFileSync(directory)) {
      const match = /^appmanifest_([1-9][0-9]{0,9})\.acf$/.exec(name);
      if (match && !apps.includes(match[1])) apps.push(match[1]);
    }
  }
  const oldFolders = `${source}/libraryfolders.vdf`;
  const folders = `${destination}/libraryfolders.vdf`;
  // After activation Steam writes the canonical file through the GNU alias.
  // Prefer it over the physical client mirror on subsequent startups.
  const active = libraryText(`${destination}/.hmosdeck-single-library-v2`).trim() === library;
  const text = mergedFolders(active ? [libraryText(folders)] :
    [libraryText(folders), libraryText(oldFolders)], steam, library, apps);
  const moves: LibraryMove[] = [];
  await planMerge(source, destination, moves, [0]);
  preserve(oldFolders, `${source}/libraryfolders.vdf.before-single-v2`);
  preserve(folders, `${destination}/libraryfolders.vdf.before-single-v2`);
  let completed = 0;
  try {
    for (const move of moves) {
      // A concurrent writer must never be overwritten; the caller runs this
      // before Steam starts, and the extra check catches accidental overlap.
      if (fs.accessSync(move.destination)) throw new Error('Steam 游戏库正在变化，请退出后重试');
      fs.renameSync(move.source, move.destination);
      completed++;
    }
  } catch (error) {
    for (let index = completed - 1; index >= 0; index--) {
      const move = moves[index];
      try { fs.renameSync(move.destination, move.source); } catch (_) {
        // No deletion: an interrupted rollback is resumable by the next run.
      }
    }
    throw new Error(`Steam 游戏库迁移失败：${(error as Error).message}`);
  }
  // A failed config write stops startup. Moved data stays safe in the canonical
  // directory, so a retry after interruption completes the same transaction.
  atomicText(folders, text);
  atomicText(oldFolders, text);
  atomicText(`${destination}/.hmosdeck-single-library-v2`, library + '\n');
  return { schemaVersion: 2, visibleLibraries: 1, movedEntries: completed, installedApps: apps.sort() };
}
