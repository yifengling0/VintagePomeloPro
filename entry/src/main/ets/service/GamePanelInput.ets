// Both the desktop Ability and its non-focusable input overlay can receive
// hardware keys. Keep one panel consumer so neither path leaks them to Wine.
export class GamePanelInput {
  private static consumer: ((code: number) => boolean) | null = null;

  static attach(consumer: (code: number) => boolean): void {
    GamePanelInput.consumer = consumer;
  }

  static detach(consumer: (code: number) => boolean): void {
    if (GamePanelInput.consumer === consumer) GamePanelInput.consumer = null;
  }

  static handle(code: number, pressed: boolean): boolean {
    if (!GamePanelInput.consumer) return false;
    if (pressed) GamePanelInput.consumer(code);
    return true;
  }
}
