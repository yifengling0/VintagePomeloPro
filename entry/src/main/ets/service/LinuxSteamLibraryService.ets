import { fileIo as fs } from '@kit.CoreFileKit';
import { LinuxSteamGame, validLinuxSteamAppId } from '../model/LinuxSteamModels';
import { parseSteamVdf } from '../steam/SteamLocalRules';
import { ConfigurationFiles } from './ConfigurationFiles';
import { readSmokeText } from './ManagedSmokePayloadService';

const ROOT_FILE = '/data/storage/el2/base/files/steamohos-native/external-root.path';
// Parse only installed manifests. Cache artwork is never proof of ownership.
export function linuxSteamGameFromManifest(text: string, id: string, library: string): LinuxSteamGame | null {
  // Shared redistributables are infrastructure, not a playable game.
  if (!validLinuxSteamAppId(id) || id === '228980') return null;
  const app = parseSteamVdf(text).child('appstate');
  if (!app || app.text('appid') !== id) return null;
  const name = app.text('name').trim();
  const directory = app.text('installdir');
  const flags = Number(app.text('stateflags'));
  if (!name || name.length > 512 || !Number.isSafeInteger(flags) || flags < 0 || (flags & 4) === 0 ||
    !directory || directory.length > 255 || /[\\/\x00-\x1f:]/.test(directory) ||
    directory === '.' || directory === '..') return null;
  const lastPlayed = Number(app.text('lastplayed'));
  return { appId: id, name, directory: `${library}/steamapps/common/${directory}`,
    cover: '', hero: '', lastPlayed: Number.isSafeInteger(lastPlayed) && lastPlayed > 0 ? lastPlayed : 0,
    updating: (flags & (2 | 256 | 512 | 1024)) !== 0 };
}

export class LinuxSteamLibraryService {
  private static instance: LinuxSteamLibraryService | null = null;
  private games: LinuxSteamGame[] = [];
  private checked: number = 0;
  private root: string = '';
  static getInstance(): LinuxSteamLibraryService {
    if (!LinuxSteamLibraryService.instance) LinuxSteamLibraryService.instance = new LinuxSteamLibraryService();
    return LinuxSteamLibraryService.instance;
  }
  externalRoot(): string {
    const root = readSmokeText(ROOT_FILE).trim();
    return root.startsWith('/') && root.endsWith('/SteamLinuxARM64') &&
      !/[\x00-\x1f]/.test(root) && !root.split('/').includes('..') ? root : '';
  }
  private image(cache: string, id: string, names: string[]): string {
    for (const name of names) {
      for (const candidate of [`${cache}/${id}/${name}`, `${cache}/${id}_${name}`]) {
        try {
          const stat = fs.lstatSync(candidate);
          if (stat.isFile() && !stat.isSymbolicLink() && stat.size > 0 && stat.size <= 16777216) return candidate;
        } catch (_) {}
      }
    }
    return `https://cdn.cloudflare.steamstatic.com/steam/apps/${id}/${names[0]}`;
  }
  scan(force: boolean = false): LinuxSteamGame[] {
    const root = this.externalRoot();
    if (!force && root === this.root && Date.now() - this.checked < 5000) return this.games.slice();
    this.root = root; this.checked = Date.now(); this.games = [];
    if (!root) return [];
    const library = `${root}/library`;
    const steamapps = `${library}/steamapps`;
    const cache = `${root}/client/.steam/steam/appcache/librarycache`;
    try {
      if (fs.lstatSync(steamapps).isSymbolicLink()) return [];
      const names = fs.listFileSync(steamapps);
      if (names.length > 10000) throw new Error('Steam 游戏库条目过多');
      const seen: Set<string> = new Set<string>();
      for (const name of names) {
        const match = /^appmanifest_([1-9][0-9]{0,9})\.acf$/.exec(name);
        if (!match || seen.has(match[1])) continue;
        try {
          const text = ConfigurationFiles.text(ConfigurationFiles.read(`${steamapps}/${name}`, 262144));
          const game = linuxSteamGameFromManifest(text, match[1], library);
          if (!game || fs.lstatSync(game.directory).isSymbolicLink() || !fs.statSync(game.directory).isDirectory()) continue;
          game.cover = this.image(cache, game.appId, ['library_600x900.jpg', 'library_capsule.jpg', 'header.jpg']);
          game.hero = this.image(cache, game.appId, ['library_hero.jpg', 'header.jpg']);
          seen.add(game.appId); this.games.push(game);
        } catch (_) { /* Steam may be atomically replacing one manifest. Retry on the next scan. */ }
      }
      this.games.sort((a: LinuxSteamGame, b: LinuxSteamGame): number =>
        b.lastPlayed - a.lastPlayed || a.name.localeCompare(b.name));
    } catch (_) { this.games = []; }
    return this.games.slice();
  }
}
