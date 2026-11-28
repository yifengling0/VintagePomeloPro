export interface OutputScaleGeometry {
  scale: number;
  boost: number;
  width: number;
  height: number;
}
// Wine font DPI is a separate session policy. Density only converts host vp.
export function resolveOutputScale(percent: number, widthPx: number, heightPx: number,
  density: number): OutputScaleGeometry {
  const scale = Math.max(75, Math.min(150, Number.isFinite(percent) ? percent : 100)) / 100;
  return { scale, boost: scale / Math.max(0.1, density),
    width: Math.max(1, Math.round(widthPx / scale)),
    height: Math.max(1, Math.round(heightPx / scale)) };
}

export const DEFAULT_WINE_DPI: number = 192;
const STANDARD_WINE_DPI: number = 96;
const MAX_WINE_DPI: number = 480;

function isPhysicalDpi(dpi: number): boolean {
  return Number.isFinite(dpi) && dpi >= 50 && dpi <= 1000;
}

export function resolveWineDpi(xDpi: number, yDpi: number, density: number,
  desktopWidth: number, desktopHeight: number): number {
  const validX = isPhysicalDpi(xDpi);
  const validY = isPhysicalDpi(yDpi);
  let dpi = DEFAULT_WINE_DPI;
  if (validX && validY) {
    dpi = (xDpi + yDpi) / 2;
  } else if (validX || validY) {
    dpi = validX ? xDpi : yDpi;
  } else if (Number.isFinite(density) && density > 0) {
    dpi = density * STANDARD_WINE_DPI;
  }

  // Reserve a dialog-sized logical desktop in either orientation. A small
  // screen at its physical DPI would otherwise crop standard Windows dialogs.
  let limit = MAX_WINE_DPI;
  if (desktopWidth > 0 && desktopHeight > 0) {
    const longEdge = Math.max(desktopWidth, desktopHeight);
    const shortEdge = Math.min(desktopWidth, desktopHeight);
    limit = Math.min(limit, Math.floor(STANDARD_WINE_DPI * Math.min(longEdge / 1024, shortEdge / 640)));
  }
  return Math.max(STANDARD_WINE_DPI, Math.min(Math.round(dpi), limit));
}
