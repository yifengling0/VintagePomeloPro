import { skinColor } from '../service/SkinColors';
import router from '@ohos.router';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { BreakpointSystem, BreakpointType, isWideBreakpoint } from '../common/BreakpointSystem';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { LengthMetrics, window } from '@kit.ArkUI';
import deviceInfo from '@ohos.deviceInfo';
import {
  applyStatusOnlyImmersiveShell,
  getMiniTitleContentTopInset,
  getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets
} from '../common/StatusOnlyImmersiveShell';
import {
  DividerShowType,
  HdsNavigation,
  HdsNavigationAttribute,
  HdsNavigationTitleMode,
  ScrollEffectType
} from '@kit.UIDesignKit';

type HelpCategory = 'start' | 'library' | 'controls' | 'runtime';

const DEVICE_SDK_API_VERSION: number = (() => {
  try {
    return deviceInfo.sdkApiVersion as number;
  } catch (_) {
    return 0;
  }
})();
const SUPPORTS_HDS_FLOATING: boolean =
  canIUse('SystemCapability.UIDesign.HDSComponent.Core') && DEVICE_SDK_API_VERSION >= 23;
const HDS_MATERIAL_TYPE_IMMERSIVE: number = 101;
const HDS_MATERIAL_LEVEL_EXQUISITE: number = 0;

interface HelpTopic {
  id: string;
  category: HelpCategory;
  icon: Resource;
  title: string;
  description: string;
  details: string;
}

interface HelpSectionData {
  id: HelpCategory;
  title: string;
  icon: Resource;
}

/**
 * Wine application's equivalent of VintagePomelo's embedded FAQ framework.
 * It deliberately uses multiple actionable topic cards instead of a one-shot
 * alert dialog so new runtime and input help can grow without changing routes.
 */
@Entry
@Component
struct HelpCenter {
  @StorageLink('libraryTitleButtonInsetVp') private titleButtonInsetVp: number = 0;
  @State private expandedTopicId: string = '';
  @State private breakpoint: BreakpointType = 'lg';
  @State private viewportWidth: number = 0;
  @State private viewportHeight: number = 0;
  @State private titleContentTopInsetVp: number = 40;
  @State private navBarInsetVp: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private breakpointCallback: ((value: BreakpointType) => void) | null = null;
  private helpScroller: Scroller = new Scroller();
  private disposeWindowInsetObserver: (() => void) | null = null;
  private readonly topics: HelpTopic[] = [
    {
      id: 'getting-started', category: 'start', icon: $r('sys.symbol.info_circle'),
      title: '开始使用 Wine 应用库', description: '了解内建工具、Windows 程序和运行状态。',
      details: '首页不会自动启动 Wine。点击需要 Wine 的卡片后才会准备运行环境；再次点击运行中的卡片可恢复现有会话。'
    },
    {
      id: 'download-library', category: 'library', icon: $r('sys.symbol.folder'),
      title: '添加 Download 中的 Windows 程序', description: '程序会从固定 games 目录自动扫描，无需目录授权。',
      details: '将 EXE 文件或游戏文件夹放入 Download/com.vintage.pomelopro/games。每个一级目录或根目录 EXE 会成为一张应用卡片；可在应用设置中选择主 EXE 与封面。'
    },
    {
      id: 'built-in-tools', category: 'library', icon: $r('sys.symbol.rectangle_grid_1x2'),
      title: '内建工具与 Windows 程序分开显示', description: 'Wine 桌面、配置、任务管理器等不会与 Download 应用混在一起。',
      details: '手机使用底部分类栏切换“内建”和“Windows”，平板与 PC 使用左侧导航。文件被移动后会保留不可用卡片，方便重新关联或删除记录。'
    },
    {
      id: 'virtual-controls', category: 'controls', icon: $r('sys.symbol.gamecontroller'),
      title: '虚拟手柄、鼠标与布局编辑', description: '顶部图标可收起或展开控制层，齿轮进入布局编辑。',
      details: '布局编辑器沿用 VintagePomelo 的方向、动作、辅助三组模型。方向组只能选择十字键或摇杆之一；可拖动分组、调整分组大小、透明度和按键显隐。'
    },
    {
      id: 'touch-modes', category: 'controls', icon: $r('sys.symbol.hand_tap'),
      title: '直触与全屏虚拟鼠标怎么选', description: '直触适合点按控件，虚拟鼠标适合精确拖动和桌面操作。',
      details: '直触模式在手指位置发送鼠标点击，双指滑动用于滚轮；打开右上角虚拟鼠标后，整块屏幕会像触摸板一样相对移动光标，轻点左键、双指轻点右键。'
    },
    {
      id: 'desktop-single', category: 'runtime', icon: $r('sys.symbol.arrow_clockwise'),
      title: '程序如何全屏运行', description: '程序统一在 Wine 桌面中运行，游戏内自行切换全屏。',
      details: '旧柚Pro 使用桌面模式运行所有 Windows 程序：保留完整 Explorer 桌面和多窗口，游戏可在自身设置中切换全屏，无需额外画面拉伸适配。'
    },
    {
      id: 'troubleshooting', category: 'runtime', icon: $r('sys.symbol.gearshape'),
      title: '程序无法启动时怎么办', description: '先检查卡片状态、主 EXE 与 Wine 引擎提示。',
      details: '在应用卡片的设置中重新关联 EXE，确认文件仍在 games 目录。若 Wine 会话卡住，可到系统设置的 Wine 引擎页停止全部 Windows 会话后重试。'
    },
    {
      id: 'first-start', category: 'runtime', icon: $r('sys.symbol.clock'),
      title: '第一次启动为什么比较慢', description: '首次使用会解压 Wine、创建 prefix 并准备图形与音频环境。',
      details: '初始化期间应用会锁定 Wine 画面并显示进度提示。请保持应用在前台，不要重复点击卡片；首次完成后，后续启动会直接复用已准备好的运行环境。'
    },
    {
      id: 'phone-layout', category: 'start', icon: $r('sys.symbol.rectangle_grid_1x2'),
      title: '手机横竖屏与小窗怎么操作', description: '手机使用顶部菜单和底部分类，大屏自动切换左侧导航。',
      details: '窗口宽度不足 960vp 时（手机，以及平板竖屏）使用顶部菜单和底部沉浸导航；达到 960vp 后（平板横屏、PC）切换为左侧导航。竖屏和横屏会根据可用宽度重新排布卡片。顶部菜单可打开系统设置、帮助和卡片视图；底部导航用于切换最近、内建应用、Windows 程序、收藏和运行中。'
    }
  ];
  private readonly sections: HelpSectionData[] = [
    { id: 'start', title: '快速开始', icon: $r('sys.symbol.info_circle') },
    { id: 'library', title: '应用库', icon: $r('sys.symbol.folder') },
    { id: 'controls', title: '输入与控制', icon: $r('sys.symbol.gamecontroller') },
    { id: 'runtime', title: '运行与排障', icon: $r('sys.symbol.gearshape') }
  ];

  aboutToAppear(): void {
    BreakpointSystem.initialize();
    this.breakpointCallback = (value: BreakpointType): void => { this.breakpoint = value; };
    BreakpointSystem.subscribe(this.breakpointCallback);
  }

  aboutToDisappear(): void {
    if (this.breakpointCallback) BreakpointSystem.unsubscribe(this.breakpointCallback);
    this.stopWindowInsetObserver();
  }

  onPageShow(): void {
    void this.applyPageWindowShell();
  }

  onPageHide(): void {
    this.stopWindowInsetObserver();
  }

  private async applyPageWindowShell(): Promise<void> {
    try {
      const pageWindow = await window.getLastWindow(this.context);
      await applyStatusOnlyImmersiveShell(pageWindow);
      this.startWindowInsetObserver(pageWindow);
    } catch (_) {
    }
  }

  private refreshTitleContentInset(pageWindow: window.Window): void {
    this.titleContentTopInsetVp = getMiniTitleContentTopInset(pageWindow);
    this.navBarInsetVp = getStatusOnlyWindowInsets(pageWindow).bottom;
  }

  private startWindowInsetObserver(pageWindow: window.Window): void {
    this.stopWindowInsetObserver();
    this.disposeWindowInsetObserver = observeStatusOnlyWindowInsets(pageWindow, () => {
      this.refreshTitleContentInset(pageWindow);
    });
  }

  private stopWindowInsetObserver(): void {
    if (this.disposeWindowInsetObserver) {
      this.disposeWindowInsetObserver();
      this.disposeWindowInsetObserver = null;
    }
  }

  private isNarrowPhone(): boolean {
    return this.breakpoint === 'xs';
  }

  private isPhoneLandscape(): boolean {
    return DeviceCapabilityPolicy.isPhoneDevice() && this.viewportWidth > this.viewportHeight && this.viewportHeight > 0;
  }

  private isLargeLayout(): boolean {
    return isWideBreakpoint(this.breakpoint);
  }

  private usesDarkAppearance(): boolean {
    try {
      return this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    } catch (_) {
      return false;
    }
  }

  private navigationTitleBackground(scrolled: boolean): string {
    if (this.usesDarkAppearance()) return scrolled ? '#80121821' : '#26121821';
    return scrolled ? '#99FFFFFF' : '#18FFFFFF';
  }

  private navigationControlBackground(): string {
    return this.usesDarkAppearance() ? '#30FFFFFF' : '#28FFFFFF';
  }

  private navigationTitleColor(): string {
    return this.usesDarkAppearance() ? '#F4F7FB' : '#182431';
  }

  private topicSurfaceColor(): string {
    return this.usesDarkAppearance() ? '#36202833' : '#DDF5F9FD';
  }

  private topicBorderColor(): string {
    return this.usesDarkAppearance() ? '#52FFFFFF' : '#8CFFFFFF';
  }

  private helpContentMaxWidth(): number {
    if (this.breakpoint === 'xl') return 1120;
    if (this.breakpoint === 'lg') return 980;
    if (this.breakpoint === 'md') return 840;
    return 100000;
  }

  private helpSidePadding(): number {
    if (this.isNarrowPhone()) return 12;
    if (this.breakpoint === 'sm') return 16;
    if (this.breakpoint === 'md') return 20;
    return 28;
  }

  private helpTopInset(): number {
    // The spacer is inside Scroll content so it scrolls away with the first
    // card instead of becoming a permanent blank band below the title.
    return this.titleContentTopInsetVp;
  }

  private titleButtonMargin(): number {
    return this.titleButtonInsetVp > 0 ? this.titleButtonInsetVp + 44 : 0;
  }

  private sectionTopics(category: HelpCategory): HelpTopic[] {
    return this.topics.filter((topic: HelpTopic): boolean => topic.category === category);
  }

  private helpColumnSections(column: number): HelpSectionData[] {
    return this.sections.filter((_section: HelpSectionData, index: number): boolean => index % 2 === column);
  }

  @Builder
  private TopicCard(topic: HelpTopic) {
    Column() {
      Row({ space: 12 }) {
        Stack({ alignContent: Alignment.Center }) {
          SymbolGlyph(topic.icon).fontSize(18).fontColor([skinColor('accent')])
        }
        .width(38).height(38).borderRadius(13)
        .backgroundColor(skinColor('selected_background'))
        Column({ space: 4 }) {
          Text(topic.title).fontSize(15).fontWeight(FontWeight.Medium)
            .fontColor(skinColor('text_primary')).width('100%')
          Text(topic.description).fontSize(11).fontColor(skinColor('text_secondary'))
            .lineHeight(17).maxLines(2).width('100%')
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
        SymbolGlyph(this.expandedTopicId === topic.id ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
          .fontSize(18).fontColor([skinColor('text_secondary')])
      }
      .width('100%').padding(16)
      if (this.expandedTopicId === topic.id) {
        Text(topic.details).fontSize(13).lineHeight(21).fontColor(skinColor('text_secondary')).width('100%')
          .padding({ left: 66, right: 16, bottom: 18 })
      }
    }
    .width('100%').backgroundColor(this.topicSurfaceColor())
    .backgroundBlurStyle(BlurStyle.COMPONENT_ULTRA_THICK).borderRadius(18)
    .border({ width: 1, color: this.topicBorderColor() })
    .onClick((): void => { this.expandedTopicId = this.expandedTopicId === topic.id ? '' : topic.id; })
  }

  @Builder
  private HelpSection(section: HelpSectionData) {
    Column({ space: 12 }) {
      Row({ space: 10 }) {
        Stack({ alignContent: Alignment.Center }) {
          SymbolGlyph(section.icon).fontSize(19).fontColor([skinColor('accent')])
        }
        .width(36).height(36).borderRadius(12)
        .backgroundColor(skinColor('selected_background'))
        Column({ space: 2 }) {
          Text(section.title).fontSize(18).fontWeight(FontWeight.Bold)
            .fontColor(skinColor('text_primary')).width('100%')
          Text(this.sectionTopics(section.id).length.toString() + ' 个主题')
            .fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
      }.width('100%')
      Column({ space: 10 }) {
        ForEach(this.sectionTopics(section.id), (topic: HelpTopic): void => {
          this.TopicCard(topic)
        }, (topic: HelpTopic): string => topic.id)
      }.width('100%')
    }.width('100%')
  }

  build() {
    Stack() {
      HdsNavigation() {
        Scroll(this.helpScroller) {
          Column({ space: this.isLargeLayout() ? 28 : 22 }) {
            if (this.isLargeLayout()) {
              Row({ space: 24 }) {
                Column({ space: 28 }) {
                  ForEach(this.helpColumnSections(0), (section: HelpSectionData): void => {
                    this.HelpSection(section)
                  }, (section: HelpSectionData): string => section.id)
                }
                .layoutWeight(1)
                .alignItems(HorizontalAlign.Start)
                Column({ space: 28 }) {
                  ForEach(this.helpColumnSections(1), (section: HelpSectionData): void => {
                    this.HelpSection(section)
                  }, (section: HelpSectionData): string => section.id)
                }
                .layoutWeight(1)
                .alignItems(HorizontalAlign.Start)
              }
              .width('100%')
              .alignItems(VerticalAlign.Top)
            } else {
              ForEach(this.sections, (section: HelpSectionData): void => {
                this.HelpSection(section)
              }, (section: HelpSectionData): string => section.id)
            }

            Row({ space: 12 }) {
              Stack({ alignContent: Alignment.Center }) {
                SymbolGlyph($r('sys.symbol.gearshape')).fontSize(18).fontColor([skinColor('accent')])
              }
              .width(40).height(40).borderRadius(14).backgroundColor(skinColor('selected_background'))
              Column({ space: 2 }) {
                Text('仍需帮助？').fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
                Text('可在系统设置中检查 Wine 引擎与输入配置。').fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)
              Button('系统设置').height(40).borderRadius(14).fontSize(13)
                .backgroundColor(skinColor('selected_background')).fontColor(skinColor('accent'))
                .onClick((): void => { router.pushUrl({ url: 'pages/SystemSettings' }); })
            }
            .width('100%').padding(14).borderRadius(18)
            .backgroundColor(this.topicSurfaceColor())
            .backgroundBlurStyle(BlurStyle.COMPONENT_ULTRA_THICK)
            .border({ width: 1, color: this.topicBorderColor() })
          }
        // Scroll content must be allowed to grow beyond the viewport. A fixed
        // 100% height makes the last topic render under the bottom inset.
        .width('100%').constraintSize({ minHeight: '100%' }).justifyContent(FlexAlign.Start)
        .padding({
          left: this.helpSidePadding(), right: this.helpSidePadding(),
          top: this.helpTopInset(),
          bottom: Math.max(this.isNarrowPhone() ? 32 : 52, this.navBarInsetVp)
        })
        .constraintSize({ maxWidth: this.helpContentMaxWidth() })
        .alignSelf(this.isLargeLayout() ? ItemAlign.Center : ItemAlign.Start)
      }
      .width('100%').height('100%').scrollBar(BarState.Off)
      }
      .mode(NavigationMode.Stack)
      .titleBar({
        padding: { start: LengthMetrics.vp(14), end: LengthMetrics.vp(14 + this.titleButtonMargin()) },
        style: {
          scrollEffectOpts: {
            enableScrollEffect: true,
            scrollEffectType: SUPPORTS_HDS_FLOATING ? ScrollEffectType.IMMERSIVE_GRADIENT_BLUR : ScrollEffectType.GRADUAL_BLUR,
            blurEffectiveStartOffset: LengthMetrics.vp(0),
            blurEffectiveEndOffset: LengthMetrics.vp(SUPPORTS_HDS_FLOATING ? 96 : 68)
        },
        originalStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(false)
          },
          contentStyle: {
              titleStyle: {
                mainTitleColor: skinColor('text_primary'),
                subTitleColor: skinColor('text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
              }
            }
        },
        scrollEffectStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(true)
          },
          contentStyle: {
              titleStyle: {
                mainTitleColor: skinColor('text_primary'),
                subTitleColor: skinColor('text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
              }
            }
        },
        systemMaterialEffect: SUPPORTS_HDS_FLOATING ? {
          materialType: HDS_MATERIAL_TYPE_IMMERSIVE,
          materialLevel: HDS_MATERIAL_LEVEL_EXQUISITE
        } : undefined
        },
        content: {
          title: {
            mainTitle: '帮助中心',
            subTitle: this.isPhoneLandscape() ? undefined : 'Wine 应用库、输入控制与 Windows 会话使用指南'
          },
          divider: { showType: DividerShowType.OFF }
      },
        avoidLayoutSafeArea: true
      })
      .titleMode(HdsNavigationTitleMode.MINI)
      .hideToolBar(true)
      .systemBarStyle(
        { statusBarContentColor: this.navigationTitleColor() },
        { statusBarContentColor: this.navigationTitleColor() }
      )
      .bindToScrollable([this.helpScroller])
      .width('100%').height('100%')
      .backgroundColor('#00FFFFFF')
    }
    .width('100%').height('100%').backgroundColor(skinColor('page_background'))
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
    .onAreaChange((_oldValue: Area, area: Area): void => {
      this.viewportWidth = area.width as number;
      this.viewportHeight = area.height as number;
      BreakpointSystem.updateFromWidth(this.viewportWidth);
    })
  }
}
