import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import fs from '@ohos.file.fs';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { fileUri, picker } from '@kit.CoreFileKit';
import { hilog } from '../service/LogService';
import {
  BOX64_APP_PRESET_IDS,
  BOX64_PRESET_DEFAULT,
  GRAPHICS_LAUNCH_PRESET_IDS,
  AppDescriptor,
  AppSettings as StoredAppSettings,
  AppSource,
  DisplayMode,
  box64PresetLabel,
  cloneAppSettings,
  createDefaultAppSettings,
  formatLaunchArgumentText,
  graphicsLaunchPresetDescription,
  graphicsLaunchPresetLabel,
  parseLaunchArgumentText
} from '../model/AppModels';
import { AppCatalogService } from '../service/AppCatalogService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { BreakpointSystem, BreakpointType } from '../common/BreakpointSystem';
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
  HdsNavigationMenuContentOptions,
  HdsNavigationTitleMode,
  ScrollEffectType
} from '@kit.UIDesignKit';

const DEVICE_SDK_API_VERSION: number = (() => {
  try {
    return deviceInfo.sdkApiVersion as number;
  } catch (_) {
    return 0;
  }
})();
const DOMAIN = 0x0000;
const TAG = 'AppSet';
const SUPPORTS_HDS_FLOATING: boolean =
  canIUse('SystemCapability.UIDesign.HDSComponent.Core') && DEVICE_SDK_API_VERSION >= 23;
const HDS_MATERIAL_TYPE_IMMERSIVE: number = 101;
const HDS_MATERIAL_LEVEL_EXQUISITE: number = 0;

class ExecutableChoice {
  path: string;
  relativePath: string;

  constructor(path: string, relativePath: string) {
    this.path = path;
    this.relativePath = relativePath;
  }
}

@CustomDialog
struct WindowsExecutablePickerDialog {
  controller?: CustomDialogController;
  choices: ExecutableChoice[] = [];
  selectedPath: string = '';
  onSelect: (path: string) => void = (_path: string): void => {};

  build() {
    Column({ space: 14 }) {
      Column({ space: 5 }) {
        Text('选择 Windows 主程序').fontSize(20).fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary')).width('100%')
        Text('仅显示当前游戏目录及其子目录中的 EXE、COM、BAT、CMD。')
          .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
      }.width('100%').alignItems(HorizontalAlign.Start)

      Scroll() {
        Column({ space: 8 }) {
          ForEach(this.choices, (choice: ExecutableChoice) => {
            Button() {
              Column({ space: 3 }) {
                Text(choice.relativePath).fontSize(14).fontWeight(FontWeight.Medium)
                  .fontColor($r('app.color.text_primary')).width('100%').maxLines(2)
                Text(choice.path === this.selectedPath ? '当前已关联' : '选择为此卡片的主程序')
                  .fontSize(11)
                  .fontColor(choice.path === this.selectedPath ? $r('app.color.accent') : $r('app.color.text_secondary'))
                  .width('100%')
              }.width('100%').alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .padding(14)
            .borderRadius(14)
            .backgroundColor(choice.path === this.selectedPath ? '#1F246BFD' : $r('app.color.search_background'))
            .onClick((): void => {
              if (this.controller) this.controller.close();
              this.onSelect(choice.path);
            })
          }, (choice: ExecutableChoice): string => choice.path)
        }.width('100%')
      }
      .width('100%')
      .constraintSize({ maxHeight: 380 })
      .scrollBar(BarState.Auto)

      Button('取消').width('100%').height(46).backgroundColor($r('app.color.search_background'))
        .fontColor($r('app.color.text_primary')).onClick((): void => {
          if (this.controller) this.controller.close();
        })
    }
    .width('92%')
    .constraintSize({ maxWidth: 620 })
    .padding(20)
    .backgroundColor($r('app.color.card_background'))
    .borderRadius(20)
  }
}

@Entry
@Component
struct AppSettingsPage {
  @StorageLink('libraryTitleButtonInsetVp') private titleButtonInsetVp: number = 0;
  @State private app: AppDescriptor | null = null;
  @State private value: StoredAppSettings = createDefaultAppSettings('');
  @State private breakpoint: BreakpointType = 'lg';
  @State private titleContentTopInsetVp: number = 40;
  @State private navBarInsetVp: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private breakpointCallback: ((value: BreakpointType) => void) | null = null;
  private appSettingsScroller: Scroller = new Scroller();
  private disposeWindowInsetObserver: (() => void) | null = null;
  private executablePickerController: CustomDialogController | null = null;
  private openExecutablePickerOnShow: boolean = false;

  private isCompact(): boolean {
    return this.breakpoint === 'xs' || this.breakpoint === 'sm';
  }

  private isNarrowPhone(): boolean {
    return this.breakpoint === 'xs';
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

  private cardPadding(): number {
    return this.isCompact() ? 12 : 20;
  }

  private contentTopInset(): number {
    // Keep the HDS title clearance inside Scroll content so it moves away
    // together with the first settings card.
    return this.titleContentTopInsetVp;
  }

  private contentSidePadding(): number {
    if (this.isNarrowPhone()) return 10;
    if (this.breakpoint === 'sm') return 16;
    if (this.breakpoint === 'md') return 20;
    return 28;
  }

  private contentMaxWidth(): number {
    return this.breakpoint === 'xl' ? 1040 : 920;
  }

  private titleButtonMargin(): number {
    return this.titleButtonInsetVp > 0 ? this.titleButtonInsetVp + 44 : 0;
  }

  private currentProfileId(): string {
    return this.value.inputProfileId || 'default';
  }

  private profileEditorId(): string {
    return this.currentProfileId() === 'default' ? 'app-' + this.value.appId : this.currentProfileId();
  }

  private profileEditorName(): string {
    const title = this.value.customTitle || this.app?.title || '应用';
    return title + ' 键鼠手柄布局';
  }

  private globalProfileId(): string {
    return AppSettingsStore.getInstance().getGlobalSettings().desktopInputProfileId || 'default';
  }

  aboutToAppear(): void {
    BreakpointSystem.initialize();
    this.breakpointCallback = (value: BreakpointType): void => { this.breakpoint = value; };
    BreakpointSystem.subscribe(this.breakpointCallback);
    const params = (router.getParams() as Record<string, string> | undefined) ?? {};
    const appId = params.appId ?? '';
    this.openExecutablePickerOnShow = params.selectExecutable === 'true';
    this.app = AppCatalogService.getInstance().getApps().find((item: AppDescriptor) => item.id === appId) ?? null;
    this.value = AppSettingsStore.getInstance().getAppSettings(appId);
    if (this.app) {
      const next = cloneAppSettings(this.value);
      next.appId = appId;
      next.customTitle = this.value.customTitle || this.app.title;
      next.executable = this.value.executable || this.app.launchTarget.executable;
      next.workingDirectory = this.value.workingDirectory || this.app.launchTarget.workingDirectory;
      next.displayModeOverride = this.value.displayModeOverride ?? this.app.displayModeOverride;
      this.value = next;
    }
  }

  aboutToDisappear(): void {
    if (this.breakpointCallback) BreakpointSystem.unsubscribe(this.breakpointCallback);
    this.stopWindowInsetObserver();
    if (this.executablePickerController) this.executablePickerController.close();
    this.executablePickerController = null;
    // 修改即保存：离开页面时兜底持久化，确保直接返回也不丢失任何修改。
    void this.persist();
  }

  onPageShow(): void {
    void this.applyPageWindowShell();
    if (this.openExecutablePickerOnShow) {
      this.openExecutablePickerOnShow = false;
      this.openExecutablePicker();
    }
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

  private replace(title: string, cover: string, executable: string, workingDirectory: string,
    displayMode: DisplayMode | null): void {
    const next = cloneAppSettings(this.value);
    next.customTitle = title;
    next.customCover = cover;
    next.executable = executable;
    next.workingDirectory = workingDirectory;
    next.displayModeOverride = displayMode;
    this.value = next;
  }

  /** 修改即保存：仅持久化设置，目录卡片在返回主页面时自动刷新。 */
  private async persist(): Promise<void> {
    await AppSettingsStore.getInstance().saveAppSettings(this.value);
  }

  /** 持久化并刷新目录缓存（封面、主程序等需要立即反映到卡片）。 */
  private async persistAndScan(message: string): Promise<void> {
    await AppSettingsStore.getInstance().saveAppSettings(this.value);
    await AppCatalogService.getInstance().scan();
    if (message.length > 0) promptAction.showToast({ message: message });
  }

  private relativeExecutablePath(directory: string, executable: string): string {
    const root = directory.replace(/\\/g, '/').replace(/\/+$/, '');
    const target = executable.replace(/\\/g, '/');
    if (root && target.startsWith(root + '/')) return target.substring(root.length + 1);
    return target;
  }

  private async selectExecutableCandidate(executable: string): Promise<void> {
    const separator = executable.lastIndexOf('/');
    const workingDirectory = separator > 0 ? executable.substring(0, separator) : this.value.workingDirectory;
    this.replace(this.value.customTitle, this.value.customCover, executable,
      workingDirectory, this.value.displayModeOverride);
    this.executablePickerController = null;
    await this.persistAndScan('已选择主程序');
  }

  private async openExecutablePicker(): Promise<void> {
    if (!this.app || this.app.source !== AppSource.DOWNLOAD) return;
    const candidates = await AppCatalogService.getInstance().getGameLaunchTargetCandidates(this.app.directory);
    if (candidates.length === 0) {
      AlertDialog.show({
        title: '未找到可启动的主程序',
        message: '已扫描当前游戏目录及两层子目录，未发现 EXE、COM、BAT 或 CMD。请确认游戏文件已完整放入此目录。',
        primaryButton: { value: '知道了', action: (): void => {} }
      });
      return;
    }
    const choices: ExecutableChoice[] = candidates.map((path: string) =>
      new ExecutableChoice(path, this.relativeExecutablePath(this.app?.directory ?? '', path)));
    this.executablePickerController = new CustomDialogController({
      builder: WindowsExecutablePickerDialog({
        choices: choices,
        selectedPath: this.value.executable,
        onSelect: (path: string): void => { void this.selectExecutableCandidate(path); }
      }),
      cancel: (): void => { this.executablePickerController = null; },
      autoCancel: true,
      alignment: DialogAlignment.Center,
      customStyle: true
    });
    try {
      this.executablePickerController.open();
    } catch (_) {
      this.executablePickerController = null;
      promptAction.showToast({ message: '无法打开主程序选择器' });
    }
  }

  private async selectCover(): Promise<void> {
    try {
      // 用图片选择器（相册）而非文件选择器：直接浏览图片，体验更符合"选择封面"。
      const photoPicker = new picker.PhotoViewPicker();
      const photoSelectOptions = new picker.PhotoSelectOptions();
      photoSelectOptions.MIMEType = picker.PhotoViewMIMETypes.IMAGE_TYPE;
      photoSelectOptions.maxSelectNumber = 1;
      const result = await photoPicker.select(photoSelectOptions);
      if (!result || result.photoUris.length === 0) return;
      const cover = await this.importCover(result.photoUris[0]);
      if (cover.length === 0) {
        promptAction.showToast({ message: '封面复制失败，请重试' });
        return;
      }
      this.replace(this.value.customTitle, cover, this.value.executable,
        this.value.workingDirectory, this.value.displayModeOverride);
      await this.persistAndScan('封面已更新');
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'selectCover failed: %{public}s', JSON.stringify(e));
      promptAction.showToast({ message: '未选择封面' });
    }
  }

  /**
   * 把选择器授权的封面图片复制到该游戏自己的目录（<gameDir>/cover.<ext>）后
   * 返回本地路径。
   * - 每个游戏存各自目录: 不同游戏互不干扰, 不会有共享目录的覆盖/串用问题;
   *   固定名 cover.png/jpg/jpeg/webp — scan 的 findCover/chooseCoverFileName
   *   自动发现, 游戏目录迁移/备份时封面跟随, 不依赖 Preferences 里的绝对路径。
   * - 同一游戏换封面时目标已存在 (cover.<ext>), 先删旧再拷避免覆盖失败。
   */
  private async importCover(uri: string): Promise<string> {
    let srcFd: number = -1;
    try {
      const dir = this.app?.directory ?? '';
      if (!dir) return '';
      // 媒体库 URI 可能带缩略图 query（?timestampApp=...&path=...），
      // 取文件名前先剥离，避免 query 里的分隔符污染扩展名。
      const cleanUri = uri.split('?')[0];
      const coverName = new fileUri.FileUri(cleanUri).name;
      const dot = coverName.lastIndexOf('.');
      const ext = dot > 0 ? coverName.substring(dot).toLowerCase() : '.png';
      const dest = dir + '/cover' + ext;
      // 同游戏换封面: 先删旧封面再拷（目标已存在时防 copyFile 覆盖失败）
      await this.removeCoverFiles(dir, dest);
      srcFd = fs.openSync(uri, fs.OpenMode.READ_ONLY).fd;
      await fs.copyFile(srcFd, dest);
      hilog.info(DOMAIN, TAG, 'cover imported: %{public}s', dest);
      return dest;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'cover import failed: %{public}s', JSON.stringify(e));
      return '';
    } finally {
      if (srcFd >= 0) {
        try { fs.closeSync(srcFd); } catch (_) {}
      }
    }
  }

  /**
   * 删除游戏目录内的旧封面文件（cover.* / folder.* 旧格式），保留 keep
   * （keep 传 '' 表示全部删除，用于"移除封面"）。
   */
  private async removeCoverFiles(dir: string, keep: string): Promise<void> {
    try {
      const keepName = keep.length > 0 ? keep.substring(keep.lastIndexOf('/') + 1) : '';
      const names: string[] = await fs.listFile(dir);
      for (let index = 0; index < names.length; index++) {
        const name = names[index];
        if (keepName.length > 0 && name === keepName) continue;
        if (/^cover\.(png|jpe?g|webp|bmp)$/i.test(name) ||
          /^folder\.(png|jpe?g|webp|bmp)$/i.test(name)) {
          try { fs.unlinkSync(dir + '/' + name); } catch (_) {}
        }
      }
    } catch (_) {
    }
  }

  /** 移除封面: 删该游戏目录的封面文件 + 清 customCover，卡片立即恢复默认封面。 */
  private async removeCover(): Promise<void> {
    try {
      if (this.app?.directory) {
        await this.removeCoverFiles(this.app.directory, '');
      }
      this.replace(this.value.customTitle, '', this.value.executable,
        this.value.workingDirectory, this.value.displayModeOverride);
      await this.persistAndScan('已移除封面');
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'removeCover failed: %{public}s', JSON.stringify(e));
      promptAction.showToast({ message: '移除封面失败，请重试' });
    }
  }

  private removeUnavailable(): void {
    if (!this.app || this.app.available) return;
    AlertDialog.show({
      title: '删除不可用卡片',
      message: '仅删除应用库记录，不会删除磁盘中的文件。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '删除', fontColor: '#D94848', action: (): void => {
          AppSettingsStore.getInstance().removeCachedApp(this.value.appId).then(() => router.back());
        }
      }
    });
  }

  private async editInputProfile(): Promise<void> {
    const profileId = this.profileEditorId();
    const next = cloneAppSettings(this.value);
    next.inputProfileId = profileId;
    this.value = next;
    // Entering a child editor is an explicit choice to create a dedicated
    // profile. Persist it first so returning from the editor cannot restore
    // stale app settings and silently lose the association.
    await AppSettingsStore.getInstance().saveAppSettings(this.value);
    router.pushUrl({
      url: 'pages/InputLayoutEditor',
      params: {
        appId: this.value.appId,
        profileId: profileId,
        profileName: this.profileEditorName(),
        baseProfileId: this.globalProfileId()
      }
    });
  }

  private async useGlobalInputProfile(): Promise<void> {
    const next = cloneAppSettings(this.value);
    next.inputProfileId = 'default';
    this.value = next;
    await AppSettingsStore.getInstance().saveAppSettings(this.value);
    promptAction.showToast({ message: '此应用已恢复使用全局输入方案' });
  }

  private setFavorite(value: boolean): void {
    const next = cloneAppSettings(this.value);
    next.favorite = value;
    this.value = next;
    void this.persist();
  }

  private setBox64Preset(preset: string): void {
    if (preset === this.value.box64Preset) return;
    const next = cloneAppSettings(this.value);
    next.box64Preset = preset;
    this.value = next;
    void this.persist();
  }

  private setGraphicsLaunchPreset(preset: string): void {
    if (preset === this.value.graphicsLaunchPreset) return;
    const next = cloneAppSettings(this.value);
    next.graphicsLaunchPreset = preset;
    this.value = next;
    void this.persist();
  }

  private setDisableMfDxgiDeviceManager(enabled: boolean): void {
    if (enabled === this.value.disableMfDxgiDeviceManager) return;
    const next = cloneAppSettings(this.value);
    next.disableMfDxgiDeviceManager = enabled;
    this.value = next;
    void this.persist();
  }

  private setPreferMfMpeg4Handler(enabled: boolean): void {
    if (enabled === this.value.preferMfMpeg4Handler) return;
    const next = cloneAppSettings(this.value);
    next.preferMfMpeg4Handler = enabled;
    this.value = next;
    void this.persist();
  }

  private setExtraLaunchArguments(text: string): void {
    const next = cloneAppSettings(this.value);
    next.arguments = parseLaunchArgumentText(text);
    this.value = next;
    void this.persist();
  }

  @Builder
  private Field(label: string, value: string, onChange: (value: string) => void) {
    Column({ space: 7 }) {
      Text(label).fontSize(13).fontColor($r('app.color.text_secondary'))
      TextInput({ text: value }).height(48).borderRadius(14).backgroundColor($r('app.color.search_background'))
        .onChange((text: string) => onChange(text))
    }.width('100%').alignItems(HorizontalAlign.Start)
  }

  build() {
    Stack() {
      HdsNavigation() {
        Scroll(this.appSettingsScroller) {
        Column({ space: 18 }) {
          Column({ space: 16 }) {
            Text('卡片与启动').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
            this.Field('显示名称', this.value.customTitle, (text: string): void => {
              this.replace(text, this.value.customCover, this.value.executable,
                this.value.workingDirectory, this.value.displayModeOverride);
              void this.persist();
            })
            Row() {
              Column({ space: 4 }) {
                Text('收藏').fontSize(15).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
                Text('收藏后可从侧边栏快速访问').fontSize(11).fontColor($r('app.color.text_secondary'))
              }.alignItems(HorizontalAlign.Start).layoutWeight(1)
              Toggle({ type: ToggleType.Switch, isOn: this.value.favorite }).onChange((value: boolean): void => this.setFavorite(value))
            }.width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            if (this.app && this.app.source === AppSource.DOWNLOAD) {
              Column({ space: 6 }) {
                Text('当前游戏目录').fontSize(13).fontColor($r('app.color.text_secondary'))
                Text(this.app.directory).fontSize(12).fontColor($r('app.color.text_primary')).maxLines(2)
                Text('从此目录及所有子目录选择 EXE、COM、BAT 或 CMD；不会打开系统文件框。')
                  .fontSize(11).fontColor($r('app.color.text_secondary'))
                Text(this.value.executable ? '当前主程序：' + this.relativeExecutablePath(this.app.directory, this.value.executable) : '尚未选择主程序')
                  .fontSize(11).fontColor($r('app.color.text_secondary')).maxLines(2)
                if (!this.app.available && this.value.executable === this.app.launchTarget.executable) {
                  Text('未找到当前主程序，请重新选择。').fontSize(12).fontColor('#D94848')
                }
                Button('选择此游戏目录中的主程序').width('100%').height(48).backgroundColor($r('app.color.search_background'))
                  .fontColor($r('app.color.text_primary')).id('app-settings-select-exe').onClick(() => this.openExecutablePicker())
              }.width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14).alignItems(HorizontalAlign.Start)
            } else {
              Text('内建系统应用由 Wine 运行时提供，不能更换 EXE。').fontSize(12).fontColor($r('app.color.text_secondary'))
                .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            }
            // 封面仅对下载游戏开放: 每个游戏的封面存各自的游戏目录 (cover.<ext>),
            // 内建应用封面是内置的, 不提供自定义。
            if (this.app?.source === AppSource.DOWNLOAD) {
              Button('选择封面图片').width('100%').height(48).backgroundColor($r('app.color.search_background'))
                .fontColor($r('app.color.text_primary')).id('app-settings-select-cover').onClick(() => this.selectCover())
              if (this.value.customCover.length > 0) {
                Button('移除封面').width('100%').height(44).margin({ top: 8 })
                  .backgroundColor('#1AD94848').fontColor('#D94848').id('app-settings-remove-cover')
                  .onClick(() => { void this.removeCover(); })
              }
            }
          }.width('100%').padding(this.cardPadding()).backgroundColor($r('app.color.card_background')).borderRadius(16)

          Column({ space: 12 }) {
            Text('启动参数').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
            Text('图形 API').fontSize(13).fontColor($r('app.color.text_secondary')).width('100%')
            Text('启动时追加命令行参数，让游戏选择 Direct3D 11 或 OpenGL。不改变系统设置中的 DXVK/VirGL 后端。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
              ForEach(GRAPHICS_LAUNCH_PRESET_IDS, (preset: string) => {
                Button(graphicsLaunchPresetLabel(preset))
                  .height(42)
                  .padding({ left: 12, right: 12 })
                  .margin({ right: 8, bottom: 8 })
                  .borderRadius(14)
                  .fontSize(12)
                  .fontColor(this.value.graphicsLaunchPreset === preset ?
                    '#FFFFFFFF' : $r('app.color.text_primary'))
                  .backgroundColor(this.value.graphicsLaunchPreset === preset ?
                    $r('app.color.accent') : $r('app.color.search_background'))
                  .id('app-settings-gfx-' + preset)
                  .onClick(() => this.setGraphicsLaunchPreset(preset))
              }, (preset: string): string => preset)
            }.width('100%')
            Text(graphicsLaunchPresetDescription(this.value.graphicsLaunchPreset))
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            this.Field('额外参数', formatLaunchArgumentText(this.value.arguments), (text: string): void => {
              this.setExtraLaunchArguments(text);
            })
            Text('空格分隔，例如 -force-gfx-direct 或 -no-cef-sandbox。与上方图形预设合并后启动。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
          }.width('100%').padding(this.cardPadding()).backgroundColor($r('app.color.card_background')).borderRadius(16)

          Column({ space: 12 }) {
            Text('兼容性').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
            Text('Box64 兼容性预设').fontSize(13).fontColor($r('app.color.text_secondary')).width('100%')
            Text('影响程序在兼容模拟层上的稳定性与性能；跟随全局表示使用系统设置中的预设。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
              ForEach(BOX64_APP_PRESET_IDS, (preset: string) => {
                Button(box64PresetLabel(preset))
                  .height(42)
                  .padding({ left: 12, right: 12 })
                  .margin({ right: 8, bottom: 8 })
                  .borderRadius(14)
                  .fontSize(12)
                  .fontColor(this.value.box64Preset === preset ?
                    '#FFFFFFFF' : $r('app.color.text_primary'))
                  .backgroundColor(this.value.box64Preset === preset ?
                    $r('app.color.accent') : $r('app.color.search_background'))
                  .id('app-settings-box64-' + preset)
                  .onClick(() => this.setBox64Preset(preset))
              }, (preset: string): string => preset)
            }.width('100%')
            Text(this.value.box64Preset === BOX64_PRESET_DEFAULT ?
              '此应用跟随全局兼容性预设。' : '此应用使用「' + box64PresetLabel(this.value.box64Preset) + '」。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Text('Media Foundation 视频').fontSize(13).fontColor($r('app.color.text_secondary'))
              .width('100%').margin({ top: 8 })
            Text('默认关闭。不要因为游戏是 Unity 就自动打开；仅在开场视频黑屏、花屏、有声无画或卡死时按需启用。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Row() {
              Column({ space: 4 }) {
                Text('禁用 Media Foundation DXGI 视频加速').fontSize(15).fontWeight(FontWeight.Medium)
                  .fontColor($r('app.color.text_primary'))
                Text('部分 Unity / MF 游戏开场视频黑屏、花屏、有声无画或卡死。开启后禁用 DXGI Device Manager，视频可能改走兼容性更高的非 DXGI 路径。')
                  .fontSize(11).fontColor($r('app.color.text_secondary'))
              }.alignItems(HorizontalAlign.Start).layoutWeight(1)
              Toggle({ type: ToggleType.Switch, isOn: this.value.disableMfDxgiDeviceManager })
                .onChange((value: boolean): void => this.setDisableMfDxgiDeviceManager(value))
            }.width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            .id('app-settings-mf-disable-dxgi')
            Row() {
              Column({ space: 4 }) {
                Text('优先 Wine MPEG4 Handler').fontSize(15).fontWeight(FontWeight.Medium)
                  .fontColor($r('app.color.text_primary'))
                Text('实验性。handler 查找失败时先试 Wine MPEG4，再回退 GStreamer。不要和无视频问题一起乱开。')
                  .fontSize(11).fontColor($r('app.color.text_secondary'))
              }.alignItems(HorizontalAlign.Start).layoutWeight(1)
              Toggle({ type: ToggleType.Switch, isOn: this.value.preferMfMpeg4Handler })
                .onChange((value: boolean): void => this.setPreferMfMpeg4Handler(value))
            }.width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            .id('app-settings-mf-prefer-mpeg4')
          }.width('100%').padding(this.cardPadding()).backgroundColor($r('app.color.card_background')).borderRadius(16)

          Column({ space: 12 }) {
            Text('输入映射').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
            Text('桌面模式始终使用系统设置中的桌面全局方案；这里的应用专用方案在启动单个程序时生效。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Text(this.currentProfileId() === 'default' ? '当前跟随桌面全局输入方案' : '当前使用应用专用输入方案')
              .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
            Button('编辑应用专用输入方案')
              .width('100%')
              .height(48)
              .backgroundColor($r('app.color.search_background'))
              .fontColor($r('app.color.text_primary'))
              .id('app-settings-edit-input-profile')
              .onClick(() => this.editInputProfile())
            if (this.currentProfileId() !== 'default') {
              Button('恢复使用全局输入方案')
                .width('100%')
                .height(48)
                .backgroundColor('#1F246BFD')
                .fontColor($r('app.color.accent'))
                .id('app-settings-use-global-input')
                .onClick((): void => { this.useGlobalInputProfile(); })
            }
          }.width('100%').padding(this.cardPadding()).backgroundColor($r('app.color.card_background')).borderRadius(16)

          if (this.app && !this.app.available) {
            Button('删除不可用卡片').height(48).fontColor('#D94848').backgroundColor($r('app.color.card_background')).onClick(() => this.removeUnavailable())
          }
        }
        .width('100%')
        .constraintSize({ minHeight: '100%', maxWidth: this.contentMaxWidth() })
        .padding({
          left: this.contentSidePadding(), right: this.contentSidePadding(),
          top: this.contentTopInset(),
          bottom: Math.max(this.isCompact() ? 32 : 44, this.navBarInsetVp)
        })
        .alignSelf(ItemAlign.Start)
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
                mainTitleColor: $r('app.color.text_primary'),
                subTitleColor: $r('app.color.text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.accent')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
              }
            }
        },
        scrollEffectStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(true)
          },
          contentStyle: {
              titleStyle: {
                mainTitleColor: $r('app.color.text_primary'),
                subTitleColor: $r('app.color.text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.accent')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
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
            mainTitle: this.app ? this.app.title + ' 设置' : '应用设置',
            subTitle: this.isCompact() ? undefined : '封面、启动方式、显示模式和专用输入布局'
          },
          menu: {
            value: [],
            maxCount: 1
          } as HdsNavigationMenuContentOptions,
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
      .bindToScrollable([this.appSettingsScroller])
      .width('100%').height('100%')
      .backgroundColor('#00FFFFFF')
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.page_background'))
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
  }
}
