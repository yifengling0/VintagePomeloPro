import { LengthMetrics, router, window } from '@kit.ArkUI';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import deviceInfo from '@ohos.deviceInfo';
import { DividerShowType, HdsNavigation, HdsNavigationAttribute, HdsNavigationTitleMode, ScrollEffectType } from '@kit.UIDesignKit';
import { MembershipSupport } from '../components/MembershipSupport';
import { applyStatusOnlyImmersiveShell, getMiniTitleContentTopInset, getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets } from '../common/StatusOnlyImmersiveShell';
import { membershipLegalText, membershipLegalTitle } from '../model/MembershipLegal';
import { ValueAddedConfig } from '../model/ValueAddedModels';
import { ConsolePalette, GlobalSettings, ThemeMode, UiSkin, createDefaultGlobalSettings } from '../model/AppModels';
import { skinColor } from '../service/SkinColors';
import { ThemeSettingsService } from '../service/ThemeSettingsService';

interface LegalParams { document?: string; }
@Entry
@Component
struct MembershipLegal {
  @State private title: string = '会员服务协议';
  @State private body: string = '';
  @State private document: string = 'membership';
  private operator: string = '';
  @State private settings: GlobalSettings = createDefaultGlobalSettings();
  @State private topInset: number = 80;
  @State private bottomInset: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private scroller: Scroller = new Scroller();
  private disposeInsets: (() => void) | null = null;
  private visible: boolean = false;
  private supportsImmersiveMaterial: boolean = deviceInfo.sdkApiVersion >= 23;

  aboutToAppear(): void {
    const document = (router.getParams() as LegalParams)?.document ?? 'membership';
    this.document = document;
    this.title = document === 'support' ? '客服与帮助' : '会员服务说明'; this.body = membershipLegalText(document, '');
    this.settings = ThemeSettingsService.getInstance().getSettings();
    void this.context.resourceManager.getRawFileContent('value_added_config.json').then((bytes: Uint8Array): void => {
      const config = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as ValueAddedConfig;
      this.operator = config.membershipOperatorName ?? '';
      this.body = membershipLegalText(this.document, this.operator);
    }).catch((): void => {});
  }
  onPageShow(): void {
    this.visible = true;
    this.settings = ThemeSettingsService.getInstance().getSettings();
    void this.restoreWindow();
  }
  onPageHide(): void { this.visible = false; this.stopObservingInsets(); }
  aboutToDisappear(): void { this.visible = false; this.stopObservingInsets(); }
  private stopObservingInsets(): void {
    if (this.disposeInsets) { this.disposeInsets(); this.disposeInsets = null; }
  }
  private darkAppearance(): boolean {
    if (this.settings.uiSkin === UiSkin.DECK) return true;
    if (this.settings.uiSkin === UiSkin.CONSOLE) return this.settings.consolePalette !== ConsolePalette.PEARL;
    if (this.settings.themeMode !== ThemeMode.SYSTEM) return this.settings.themeMode === ThemeMode.DARK;
    return this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
  }
  private titleBackground(scrolled: boolean): string {
    return this.darkAppearance() ? (scrolled ? '#80121821' : '#26121821') : (scrolled ? '#99FFFFFF' : '#18FFFFFF');
  }
  private controlBackground(): string { return this.darkAppearance() ? '#30FFFFFF' : '#28FFFFFF'; }
  private surface(): string | Resource {
    if (!this.settings.useGlassEffect) return skinColor('card_background');
    return this.darkAppearance() ? '#3A202833' : '#DDF5F9FD';
  }
  private cardBorder(): string { return this.darkAppearance() ? '#52FFFFFF' : '#8CFFFFFF'; }
  private selectDocument(document: string): void {
    this.document = document; this.body = membershipLegalText(document, this.operator);
    this.scroller.scrollTo({ xOffset: 0, yOffset: 0 });
  }
  @Builder
  private DocumentTab(label: string, document: string) {
    Button(label).fontSize(14).height(40).layoutWeight(1)
      .backgroundColor(this.document === document ? skinColor('selected_background') : Color.Transparent)
      .fontColor(this.document === document ? skinColor('accent') : skinColor('text_secondary'))
      .onClick((): void => this.selectDocument(document)).id('membership-tab-' + document)
  }
  private async restoreWindow(): Promise<void> {
    try {
      const pageWindow = await window.getLastWindow(this.context);
      if (!this.visible) return;
      await applyStatusOnlyImmersiveShell(pageWindow);
      if (!this.visible) return;
      await pageWindow.setWindowSystemBarProperties({ statusBarColor: '#00000000', navigationBarColor: '#00000000',
        isStatusBarLightIcon: this.darkAppearance(), isNavigationBarLightIcon: this.darkAppearance() });
      if (!this.visible) return;
      this.stopObservingInsets();
      this.disposeInsets = observeStatusOnlyWindowInsets(pageWindow, (): void => {
        this.topInset = getMiniTitleContentTopInset(pageWindow);
        this.bottomInset = getStatusOnlyWindowInsets(pageWindow).bottom;
      });
    } catch (_) { }
  }
  build() {
    HdsNavigation() {
      Scroll(this.scroller) {
        Column({ space: 20 }) {
          if (this.document !== 'support') {
            Row({ space: 8 }) {
              this.DocumentTab('会员服务', 'membership')
              this.DocumentTab('自动续费', 'renewal')
              this.DocumentTab('取消与退款', 'cancel')
            }.width('100%').padding(6).borderRadius(18).backgroundColor(this.surface())
          }
          Column() {
            Text(membershipLegalTitle(this.document)).fontSize(20).fontWeight(FontWeight.Bold)
              .fontColor(skinColor('text_primary')).width('100%').margin({ bottom: 16 })
            Text(this.body).fontSize(16).lineHeight(28).fontColor(skinColor('text_primary')).width('100%')
              .copyOption(CopyOptions.LocalDevice).id('membership-legal-body')
          }.width('100%').padding(22).borderRadius(20).backgroundColor(this.surface())
            .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
            .border({ width: 1, color: this.cardBorder() })
          Column() {
            MembershipSupport({ showHelpLink: this.document !== 'support' })
          }.width('100%').padding(22).borderRadius(20).backgroundColor(this.surface())
            .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
            .border({ width: 1, color: this.cardBorder() })
        }.width('100%').constraintSize({ maxWidth: 960 })
          .padding({ left: 20, right: 20, top: this.topInset + 16, bottom: Math.max(28, this.bottomInset) })
      }.width('100%').height('100%').scrollBar(BarState.Off).edgeEffect(EdgeEffect.Spring)
    }
    .mode(NavigationMode.Stack)
    .titleBar({
      padding: { start: LengthMetrics.vp(14), end: LengthMetrics.vp(14) },
      style: {
        scrollEffectOpts: { enableScrollEffect: true,
          scrollEffectType: this.supportsImmersiveMaterial ? ScrollEffectType.IMMERSIVE_GRADIENT_BLUR : ScrollEffectType.GRADUAL_BLUR,
          blurEffectiveStartOffset: LengthMetrics.vp(0), blurEffectiveEndOffset: LengthMetrics.vp(96) },
        originalStyle: { backgroundStyle: { backgroundColor: this.titleBackground(false) },
          contentStyle: { titleStyle: { mainTitleColor: skinColor('text_primary') },
            backIconStyle: { backgroundColor: this.controlBackground(), iconColor: skinColor('text_primary') } } },
        scrollEffectStyle: { backgroundStyle: { backgroundColor: this.titleBackground(true) },
          contentStyle: { titleStyle: { mainTitleColor: skinColor('text_primary') },
            backIconStyle: { backgroundColor: this.controlBackground(), iconColor: skinColor('text_primary') } } },
        systemMaterialEffect: this.supportsImmersiveMaterial ? { materialType: 101, materialLevel: 0 } : undefined
      },
      content: { title: { mainTitle: this.title }, divider: { showType: DividerShowType.OFF },
        backIcon: { icon: $r('sys.symbol.chevron_left'), label: '返回', componentId: 'membership-legal-back',
          action: (): void => router.back() } },
      avoidLayoutSafeArea: true
    })
    .titleMode(HdsNavigationTitleMode.MINI).hideToolBar(true).bindToScrollable([this.scroller])
    .systemBarStyle({ statusBarContentColor: this.darkAppearance() ? '#F4F7FB' : '#182431' },
      { statusBarContentColor: this.darkAppearance() ? '#F4F7FB' : '#182431' })
    .width('100%').height('100%').backgroundColor(skinColor('page_background'))
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
  }
}
