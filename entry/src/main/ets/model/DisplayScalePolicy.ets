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
