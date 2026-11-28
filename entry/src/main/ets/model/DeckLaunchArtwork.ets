export interface DeckCoverBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface DeckLaunchArtwork {
  cover: string;
  builtin?: Resource;
  origin?: DeckCoverBounds;
}

export function copyDeckCoverBounds(bounds?: DeckCoverBounds): DeckCoverBounds | undefined {
  return bounds ? { x: bounds.x, y: bounds.y, width: bounds.width, height: bounds.height } : undefined;
}
