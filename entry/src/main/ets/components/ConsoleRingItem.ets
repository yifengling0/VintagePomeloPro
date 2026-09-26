import { AppDescriptor, ConsoleCardLayout } from '../model/AppModels';
import { GameArtworkService } from '../service/GameArtworkService';
import { ConsoleGameCard } from './ConsoleGameCard';

/** Keep one ArkUI component per game while its position changes around the ring. */
@Component
export struct ConsoleRingItem {
  @Prop app: AppDescriptor;
  @Prop virtualIndex: number = 0;
  @Link scrollOffset: number;
  @Prop cellWidth: number = 240;
  @Prop viewportWidth: number = 720;
  @Prop leadingInset: number = 0;
  @Prop areaHeight: number = 480;
  @Prop viewportHeight: number = 720;
  @Prop running: boolean = false;
  @Prop favorite: boolean = false;
  @Prop lastPlayed: string = '';
  @Link @Watch('updateBackgroundPath') artworkRevision: number;
  @State private resolvedBackgroundPath: string = '';
  onSelect: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onLaunch: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onDetails: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onVisible: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};

  private ringOffset(): number {
    if (this.cellWidth <= 0) return 0;
    const center = this.leadingInset + this.virtualIndex * this.cellWidth +
      this.cellWidth / 2 - this.scrollOffset;
    return (center - this.viewportWidth / 2) / this.cellWidth;
  }

  private centerWidth(): number {
    const height = this.areaHeight || Math.max(220, this.viewportHeight * 0.55);
    return Math.max(72, Math.min(460, this.cellWidth * 1.10, (height - 14) / 1.16));
  }

  private centerHeight(): number {
    const height = this.areaHeight || Math.max(220, this.viewportHeight * 0.55);
    return Math.max(72, Math.min(this.centerWidth() * 1.16, height - 14));
  }

  private updateBackgroundPath(): void {
    this.resolvedBackgroundPath = GameArtworkService.getInstance().resolve(this.app);
  }

  build() {
    ConsoleGameCard({
      app: this.app,
      layout: ConsoleCardLayout.FOCUS_3D,
      backgroundPath: this.resolvedBackgroundPath,
      focused: Math.abs(this.ringOffset()) < 0.5,
      running: this.running,
      favorite: this.favorite,
      lastPlayed: this.lastPlayed,
      cardWidth: this.centerWidth(),
      cardHeight: this.centerHeight(),
      inRing: true,
      ringPosition: this.ringOffset(),
      ringShiftX: 0,
      onSelect: (app: AppDescriptor): void => this.onSelect(app),
      onLaunch: (app: AppDescriptor): void => this.onLaunch(app),
      onDetails: (app: AppDescriptor): void => this.onDetails(app),
      onVisible: (app: AppDescriptor): void => this.onVisible(app)
    })
    .onAppear((): void => this.updateBackgroundPath())
  }
}
