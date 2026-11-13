import { curves } from '@kit.ArkUI';

// DroidDeck Motion: same Bezier and unit-mass Compose spring parameters.
// ArkUI expects a damping coefficient, while Compose specifies a damping ratio.
export class DeckMotion {
  static readonly ease = curves.cubicBezierCurve(0.2, 0.8, 0.2, 1);
  static readonly floatEase = curves.cubicBezierCurve(0.4, 0, 0.2, 1);
  static readonly button = curves.interpolatingSpring(0, 1, 1500, 2 * 0.5 * Math.sqrt(1500));
  static readonly tile = curves.interpolatingSpring(0, 1, 1500, 2 * 0.55 * Math.sqrt(1500));
  static readonly tileLift = curves.interpolatingSpring(0, 1, 400, 2 * 0.6 * Math.sqrt(400));
  static readonly sheet = curves.interpolatingSpring(0, 1, 380, 2 * 0.7 * Math.sqrt(380));
  static readonly sheetClose = curves.interpolatingSpring(0, 1, 380, 2 * Math.sqrt(380));
  static readonly pullReturn = curves.interpolatingSpring(0, 1, 500, 2 * Math.sqrt(500));
}
