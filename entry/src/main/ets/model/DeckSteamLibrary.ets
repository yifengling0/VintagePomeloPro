import { AppDescriptor, LaunchKind } from './AppModels';
import { LinuxSteamGame, validLinuxSteamAppId } from './LinuxSteamModels';

// Source belongs to the installed library, not to the game's target OS.
// Linux Steam can also launch Windows games through Proton.
export interface DeckSteamGame extends LinuxSteamGame {
  key: string;
  backend: 'linux' | 'windows';
  windowsCard?: AppDescriptor;
}

export function mergeDeckSteamGames(linux: LinuxSteamGame[], windows: AppDescriptor[]): DeckSteamGame[] {
  const games: DeckSteamGame[] = [];
  const seen: Set<string> = new Set<string>();
  for (const game of linux) {
    const key = 'linux-' + game.appId;
    if (!validLinuxSteamAppId(game.appId) || seen.has(key)) continue;
    seen.add(key);
    games.push({ key, backend: 'linux', appId: game.appId, name: game.name, directory: game.directory,
      cover: game.cover, hero: game.hero, lastPlayed: game.lastPlayed, updating: game.updating });
  }
  for (const card of windows) {
    const appId = card.steam?.appId ?? '';
    const key = 'windows-' + appId;
    if (!card.available || card.launchTarget.kind !== LaunchKind.STEAM_GAME ||
      card.steam?.state !== 'installed' || !validLinuxSteamAppId(appId) || appId === '228980' || seen.has(key)) continue;
    seen.add(key);
    games.push({ key, backend: 'windows', windowsCard: card, appId, name: card.title, directory: card.directory,
      cover: card.coverPath, hero: card.backgroundPath ?? '', lastPlayed: 0, updating: false });
  }
  return games.sort((a: DeckSteamGame, b: DeckSteamGame): number =>
    b.lastPlayed - a.lastPlayed || a.name.localeCompare(b.name) || a.key.localeCompare(b.key));
}
