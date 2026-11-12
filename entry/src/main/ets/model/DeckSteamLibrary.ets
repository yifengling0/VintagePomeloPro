import { AppDescriptor, LaunchKind } from './AppModels';
import { isSteamAppId } from '../steam/SteamLocalRules';

export interface DeckSteamGame {
  key: string;
  appId: string;
  name: string;
  directory: string;
  cover: string;
  hero: string;
  windowsCard: AppDescriptor;
}

// Preserve the original descriptor: user settings, library path and Steam identity
// must reach the same Windows launcher used by the classic interface.
export function windowsDeckSteamGames(cards: AppDescriptor[]): DeckSteamGame[] {
  const games: DeckSteamGame[] = [];
  const seen: Set<string> = new Set<string>();
  for (const card of cards) {
    const appId = card.steam?.appId ?? '';
    if (!card.available || card.launchTarget.kind !== LaunchKind.STEAM_GAME ||
      card.steam?.state !== 'installed' || !isSteamAppId(appId) || appId === '228980' || seen.has(appId)) continue;
    seen.add(appId);
    games.push({ key: 'windows-' + appId, windowsCard: card, appId, name: card.title, directory: card.directory,
      cover: card.coverPath, hero: card.backgroundPath ?? '' });
  }
  return games.sort((a: DeckSteamGame, b: DeckSteamGame): number =>
    a.name.localeCompare(b.name) || a.key.localeCompare(b.key));
}
