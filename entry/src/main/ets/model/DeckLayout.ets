export interface DeckLayout {
  portrait: boolean;
  railWidth: number;
  bottomHeight: number;
  contentWidth: number;
  contentHeight: number;
  inset: number;
  gap: number;
  columns: number;
  coverHeight: number;
}

// Use the available ArkUI area, including split windows, rather than device type.
export function deckLayout(width: number, height: number): DeckLayout {
  const w = Number.isFinite(width) && width > 0 ? width : 1000;
  const h = Number.isFinite(height) && height > 0 ? height : 650;
  const portrait = w < 600 && h > w;
  const railWidth = portrait ? 0 : w < 700 || h < 460 ? 64 : 84;
  const bottomHeight = portrait ? 72 : 0;
  const contentWidth = Math.max(1, w - railWidth);
  const contentHeight = Math.max(1, h - bottomHeight);
  const inset = w < 600 ? 16 : w < 1000 ? 24 : 44;
  const gap = portrait ? 12 : 22;
  const available = Math.max(1, contentWidth - 2 * inset);
  const columns = Math.max(1, Math.min(portrait ? 2 : 12,
    Math.floor((available + gap) / ((portrait ? 132 : 156) + gap))));
  return { portrait, railWidth, bottomHeight, contentWidth, contentHeight, inset, gap, columns,
    coverHeight: Math.max(1, (available - (columns - 1) * gap) / columns * 1.5) };
}
