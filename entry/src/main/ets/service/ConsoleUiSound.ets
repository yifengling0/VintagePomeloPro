import { common } from '@kit.AbilityKit';
import { audio } from '@kit.AudioKit';
import { media } from '@kit.MediaKit';

/** Small, optional navigation cues. A failed audio service never blocks UI input. */
export class ConsoleUiSound {
  private static instance: ConsoleUiSound = new ConsoleUiSound();
  private pool: media.SoundPool | null = null;
  private focusId: number = 0;
  private selectId: number = 0;
  private starting: Promise<void> | null = null;
  private lastFocusAt: number = 0;

  static getInstance(): ConsoleUiSound { return ConsoleUiSound.instance; }

  initialize(context: common.UIAbilityContext): void {
    if (this.pool || this.starting) return;
    this.starting = this.load(context).finally((): void => { this.starting = null; });
  }

  private async load(context: common.UIAbilityContext): Promise<void> {
    try {
      const renderer: audio.AudioRendererInfo = {
        usage: audio.StreamUsage.STREAM_USAGE_GAME, rendererFlags: 0
      };
      const pool = await media.createSoundPool(3, renderer);
      this.pool = pool;
      const focus = context.resourceManager.getRawFdSync('ui_focus.wav');
      try { this.focusId = await pool.load(focus.fd, focus.offset, focus.length); }
      finally { context.resourceManager.closeRawFdSync('ui_focus.wav'); }
      const select = context.resourceManager.getRawFdSync('ui_select.wav');
      try { this.selectId = await pool.load(select.fd, select.offset, select.length); }
      finally { context.resourceManager.closeRawFdSync('ui_select.wav'); }
    } catch (_) {
      this.focusId = 0;
      this.selectId = 0;
    }
  }

  focus(): void {
    const now = Date.now();
    if (now - this.lastFocusAt < 90) return;
    this.lastFocusAt = now;
    if (this.pool && this.focusId > 0) {
      void this.pool.play(this.focusId, { leftVolume: 0.32, rightVolume: 0.32 }).catch((): void => {});
    }
  }

  select(): void {
    if (this.pool && this.selectId > 0) {
      void this.pool.play(this.selectId, { leftVolume: 0.4, rightVolume: 0.4 }).catch((): void => {});
    }
  }
}
