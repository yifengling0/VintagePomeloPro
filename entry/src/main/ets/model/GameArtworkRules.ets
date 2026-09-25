export interface GameMatchCandidate {
  id: number;
  name: string;
}

export function normalizeGameTitle(value: string): string {
  return value.toLowerCase().replace(/[\s\-_·.,:：!！?？()（）\[\]【】™®©]/g, '');
}

const EDITION_SUFFIXES: string[] = [
  'gameoftheyearedition', 'gameoftheyear', 'gotyedition', 'goty',
  'definitiveedition', 'completeedition', 'ultimateedition', 'deluxeedition',
  'collectorsedition', 'remasterededition', 'hdremaster', 'remastered', 'remaster'
];

function normalizeArtworkIdentity(value: string): string {
  let normalized = normalizeGameTitle(value);
  for (let index = 0; index < EDITION_SUFFIXES.length; index++) {
    const suffix = EDITION_SUFFIXES[index];
    if (normalized.length > suffix.length && normalized.endsWith(suffix)) {
      normalized = normalized.substring(0, normalized.length - suffix.length);
      break;
    }
  }
  return normalized;
}

/**
 * Steam localizes a store result's name. Search the language most likely to
 * preserve the title first, then fall back to English. A Latin title such as
 * "RichMan 8" must not be compared only with the Simplified Chinese result
 * "大富翁8".
 */
export function steamSearchLocales(title: string): string[] {
  if (/[\u3040-\u30ff]/.test(title)) return ['japanese', 'english'];
  if (/[\u3400-\u9fff]/.test(title)) return ['schinese', 'tchinese', 'japanese', 'english'];
  if (/[\uac00-\ud7af]/.test(title)) return ['koreana', 'english'];
  if (/[\u0400-\u04ff]/.test(title)) return ['russian', 'english'];
  return ['english'];
}

/** Search suggestions are never artwork identities unless one exact title remains. */
export function uniqueExactGameId(items: GameMatchCandidate[], title: string): number {
  const expected = normalizeGameTitle(title);
  if (!expected) return 0;
  let match = 0;
  for (let index = 0; index < items.length; index++) {
    const item = items[index];
    if (item.id <= 0 || normalizeGameTitle(item.name) !== expected) continue;
    if (match > 0) return 0;
    match = item.id;
  }
  return match;
}

/**
 * Accept one well-known edition suffix only when it still leaves exactly one
 * candidate. This covers store names such as "Tomb Raider Game of the Year"
 * without ever selecting an arbitrary first search result.
 */
export function uniqueArtworkGameId(items: GameMatchCandidate[], title: string): number {
  const exact = uniqueExactGameId(items, title);
  if (exact > 0) return exact;
  const expected = normalizeArtworkIdentity(title);
  if (!expected) return 0;
  let match = 0;
  for (let index = 0; index < items.length; index++) {
    const item = items[index];
    if (item.id <= 0 || normalizeArtworkIdentity(item.name) !== expected) continue;
    if (match > 0 && match !== item.id) return 0;
    match = item.id;
  }
  return match;
}
