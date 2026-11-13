import { skinColor } from '../service/SkinColors';

// Shared classic-sidebar actions; each destination reuses the existing purchase flow.
@Component
export struct LauncherSupportActions {
  @Prop collapsed: boolean = false;
  @Prop idPrefix: string = 'classic';
  onMembership: () => void = (): void => {};
  onSupport: () => void = (): void => {};
  @Builder
  private Entry(label: string, membership: boolean, callback: () => void) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 12 }) {
        Stack({ alignContent: Alignment.Center }) {
          if (membership) Image($r('app.media.vip_crown')).width(19).height(19).fillColor(skinColor('accent'))
          else SymbolGlyph($r('sys.symbol.heart')).fontSize(19).fontColor([skinColor('text_secondary')])
        }.width(20).height(24)
        if (!this.collapsed) Text(label).fontSize(14).fontColor(skinColor('text_primary')).maxLines(1)
      }.width('100%').height('100%').padding({ left: this.collapsed ? 0 : 15, right: this.collapsed ? 0 : 12 })
        .justifyContent(this.collapsed ? FlexAlign.Center : FlexAlign.Start).alignItems(VerticalAlign.Center)
    }.width('100%').height(48).padding(0).borderRadius(12).backgroundColor(Color.Transparent)
      .onClick(callback).accessibilityText(label).id(this.idPrefix + (membership ? '-membership' : '-support'))
  }
  build() {
    Column({ space: 4 }) {
      this.Entry('会员', true, this.onMembership)
      this.Entry('赞助', false, this.onSupport)
    }.width('100%')
  }
}
