export interface GameMatchCandidate {
  id: number;
  name: string;
}

export function normalizeGameTitle(value: string): string {
  return value.toLowerCase().replace(/[\s\-_·.,:：!！?？()（）\[\]【】]/g, '');
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
