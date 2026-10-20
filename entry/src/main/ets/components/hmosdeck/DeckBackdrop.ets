import { LinuxSteamGame } from '../../model/LinuxSteamModels';
import { LinuxSteamArtworkService } from '../../service/LinuxSteamArtworkService';
import { DeckArtwork } from './DeckArtwork';

// ArkUI compositor animations: no JavaScript frame loop and no Android runtime.
@Component
export struct DeckBackdrop {
  @Prop games: LinuxSteamGame[] = [];
  @Prop widthVp: number = 1000;
  @Prop heightVp: number = 650;
  @Prop portrait: boolean = false;
  @Prop artRevision: number = 0;
  @Prop @Watch('motionChanged') moving: boolean = false;
  @State private float0: number = 0;
  @State private float1: number = 0;
  @State private float2: number = 0;
  @State private drift: number = 0;
  private ready: boolean = false;
  private motionTimer: number = -1;
  aboutToAppear(): void { this.ready = true; this.motionChanged(); }
  aboutToDisappear(): void {
    this.ready = false;
    if (this.motionTimer >= 0) clearTimeout(this.motionTimer);
    this.motionTimer = -1;
  }
  private motionChanged(): void {
    if (this.motionTimer >= 0) clearTimeout(this.motionTimer);
    this.motionTimer = -1;
    if (!this.ready) return;
    this.getUIContext().animateTo({ duration: 0 }, (): void => {
      this.float0 = 0; this.float1 = 0; this.float2 = 0; this.drift = 0;
    });
    if (!this.moving) return;
    // aboutToAppear precedes build. Commit the initial transforms before animating.
    this.motionTimer = setTimeout((): void => {
      this.motionTimer = -1;
      if (!this.ready || !this.moving) return;
      this.startMotion();
    }, 50);
  }
  private startMotion(): void {
    this.getUIContext().animateTo({ duration: 9000, iterations: -1, playMode: PlayMode.Alternate, curve: Curve.EaseInOut }, (): void => { this.float0 = 1; });
    this.getUIContext().animateTo({ duration: 11000, iterations: -1, playMode: PlayMode.Alternate, curve: Curve.EaseInOut }, (): void => { this.float1 = 1; });
    this.getUIContext().animateTo({ duration: 13000, iterations: -1, playMode: PlayMode.Alternate, curve: Curve.EaseInOut }, (): void => { this.float2 = 1; });
    this.getUIContext().animateTo({ duration: 40000, iterations: -1, curve: Curve.Linear }, (): void => { this.drift = 1; });
  }
  private floatAt(index: number): number { return index === 0 ? this.float0 : index === 1 ? this.float1 : this.float2; }
  private fanHeight(index: number): number {
    return this.portrait ? Math.min(this.heightVp * 0.43, this.widthVp * 0.85) * [1, 0.9, 0.8][index] :
      this.heightVp * [0.62, 0.56, 0.50][index];
  }
  private fanX(index: number): number {
    return this.portrait ? this.widthVp * [0.25, 0.48, 0.09][index] : this.widthVp * [0.54, 0.70, 0.40][index];
  }
  private cover(game: LinuxSteamGame): string {
    // Reading the revision makes an asynchronously committed cache entry reactive.
    return this.artRevision >= 0 ? LinuxSteamArtworkService.getInstance().resolve(game.appId, game.cover) : '';
  }
  private wallGame(col: number, row: number): LinuxSteamGame { return this.games[(col * 5 + (row % 6) * 3) % this.games.length]; }
  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Column().width('100%').height('100%')
        .radialGradient({ center: ['68%', '45%'], radius: '65%', colors: [['#241A9FFF', 0], ['#001A9FFF', 1]] })
      if (this.games.length > 0 && this.games.length < 4) {
        ForEach(this.games.slice(0, 3).reverse(), (game: LinuxSteamGame, index: number) => {
          DeckArtwork({ name: game.name, source: this.cover(game), appId: game.appId })
            .width(this.fanHeight(this.games.length - 1 - index) * 2 / 3).height(this.fanHeight(this.games.length - 1 - index))
            .shadow({ radius: 28, color: '#99000000', offsetY: 18 })
            .rotate({ angle: [-9, 5, -17][this.games.length - 1 - index] + 2 * this.floatAt(this.games.length - 1 - index) })
            .position({ x: this.fanX(this.games.length - 1 - index),
              y: this.heightVp * [0.14, 0.24, 0.30][this.games.length - 1 - index] - 12 * this.floatAt(this.games.length - 1 - index) })
        }, (game: LinuxSteamGame): string => game.appId)
      } else if (this.games.length >= 4) {
        Row({ space: 14 }) {
          ForEach([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], (col: number) => {
            Column({ space: 14 }) {
              ForEach([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], (row: number) => {
                DeckArtwork({ name: this.wallGame(col, row).name, source: this.cover(this.wallGame(col, row)), appId: this.wallGame(col, row).appId })
                  .width(112).height(168)
              }, (row: number): string => row.toString())
            }.translate({ y: col % 2 === 0 ? -1092 * this.drift : 1092 * this.drift - 1092 })
          }, (col: number): string => col.toString())
        }.rotate({ angle: -13 }).position({ x: -70, y: -180 })
      } else {
        // An empty catalogue has no invented games or ownership claims.
        Column().width(this.heightVp * 0.38).height(this.heightVp * 0.56).borderRadius(18)
          .border({ width: 1, color: '#343A43' })
          .linearGradient({ angle: 145, colors: [['#1C2B3A', 0], ['#10141B', 1]] })
          .rotate({ angle: -12 + 2 * this.float0 }).position({ x: this.widthVp * (this.portrait ? 0.18 : 0.62), y: this.heightVp * 0.16 - 12 * this.float0 })
        Column().width(this.heightVp * 0.38).height(this.heightVp * 0.56).borderRadius(18)
          .border({ width: 1, color: '#343A43' }).backgroundColor('#121820')
          .rotate({ angle: 8 - this.float1 }).position({ x: this.widthVp * (this.portrait ? 0.42 : 0.76), y: this.heightVp * 0.22 - 12 * this.float1 })
      }
      Column().width('100%').height('100%')
        .linearGradient({ angle: this.portrait ? 0 : 90, colors: [['#F70A0B0D', 0], ['#DB0A0B0D', 0.34], ['#400A0B0D', 0.68], ['#1A0A0B0D', 1]] })
      Column().width('100%').height('100%')
        .linearGradient({ angle: 180, colors: [['#000A0B0D', 0.55], ['#E60A0B0D', 1]] })
    }.width('100%').height('100%').clip(true).hitTestBehavior(HitTestMode.None).accessibilityLevel('no-hide-descendants')
  }
}
