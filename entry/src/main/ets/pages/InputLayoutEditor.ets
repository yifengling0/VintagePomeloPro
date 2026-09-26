import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import { common, ConfigurationConstant, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import {
  ControlElement,
  ControlElementShape,
  ControlElementType,
  InputBinding,
  InputBindingKind,
  InputProfile,
  ThemeMode,
  UiSkin,
  cloneControlElement,
  cloneInputProfile,
  cloneAppSettings,
  createControlElement,
  createDefaultInputProfile,
  keyBinding,
  MOUSE_LEFT_BUTTON,
  MOUSE_RIGHT_BUTTON,
  MOUSE_MIDDLE_BUTTON
} from '../model/AppModels';
import { KEY_NAME_OPTIONS, keyNameLabel } from '../common/EvdevKeyNames';
import { VirtualInputOverlay } from '../components/VirtualInputOverlay';
import { InputProfileService } from '../service/InputProfileService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { consoleColors } from '../service/SkinColors';
import { hilog } from '../service/LogService';

interface PickerOption {
  label: string;
  name: string;
  kind: InputBindingKind;
  code: number;
}

const DOMAIN = 0x0000;

interface ElementPatch {
  x?: number;
  y?: number;
  scale?: number;
  widthScale?: number;
  text?: string;
  shape?: ControlElementShape;
  toggleSwitch?: boolean;
  opacity?: number;
  bindings?: InputBinding[];
  keyList?: number[];
  orientation?: string;
  outputMode?: string;
}

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(max, value));
}

function elementTypeLabel(type: ControlElementType): string {
  if (type === ControlElementType.PANEL) return '布局外框';
  if (type === ControlElementType.D_PAD) return '方向键';
  if (type === ControlElementType.STICK) return '摇杆';
  if (type === ControlElementType.TRACKPAD) return '触摸板';
  if (type === ControlElementType.RANGE_BUTTON) return '滚动键';
  return '按钮';
}

/**
 * Winlator 风格的全屏画布输入编辑器。元素使用归一化坐标自由排布：
 * 点按选中、单指拖动、双指缩放，选中后在属性面板编辑绑定/形状/透明度。
 */
@Entry
@Component
struct InputLayoutEditor {
  @State private profile: InputProfile = createDefaultInputProfile();
  @State private profileId: string = 'default';
  @State private profileName: string = '默认输入方案';
  @State private appId: string = '';
  @State private baseProfileId: string = 'default';
  @State private previewW: number = 0;
  @State private previewH: number = 0;
  @State private selectedElementId: string = '';
  @State private previewRevision: number = 0;
  private gridContext: CanvasRenderingContext2D = new CanvasRenderingContext2D();
  @State private showPropertyPanel: boolean = false;
  @State private pickerOpen: boolean = false;
  @State private pickerTargetSlot: number = -1;
  @State private pickerTargetKeyList: boolean = false;
  @State private customKeyText: string = '';
  @State private panelX: number = -1;
  @State private panelY: number = 64;
  @State private toolbarX: number = -1;
  @State private toolbarY: number = 16;
  @State private returnAbilityName: string = '';
  @State private returnSessionId: string = '';
  @State private returnToplevelId: string = '0';
  @State private safeAreaTop: number = 0;
  @State private safeAreaBottom: number = 0;
  @State private safeAreaLeft: number = 0;
  @State private safeAreaRight: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;

  private editorAccent(): string {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    return settings.uiSkin === UiSkin.CONSOLE ? consoleColors(settings.consolePalette).accent : '#FF64B5F6';
  }

  private editorSelectedBackground(): string {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    return settings.uiSkin === UiSkin.CONSOLE ?
      consoleColors(settings.consolePalette).selected_background : '#335C8DFF';
  }

  private editorBindingText(): string {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    return settings.uiSkin === UiSkin.CONSOLE ?
      consoleColors(settings.consolePalette).text_primary : '#FFFFFFFF';
  }

  private editorChipColor(classicColor: string): string {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    return settings.uiSkin === UiSkin.CONSOLE ? consoleColors(settings.consolePalette).accent : classicColor;
  }
  private panelDragStartX: number = 0;
  private panelDragStartY: number = 0;
  private panelDragTouchId: number = -1;
  private panelTouchStartX: number = 0;
  private panelTouchStartY: number = 0;
  private toolbarDragStartX: number = 0;
  private toolbarDragStartY: number = 0;
  private toolbarDragTouchId: number = -1;
  private toolbarTouchStartX: number = 0;
  private toolbarTouchStartY: number = 0;
  private originalOrientation: window.Orientation = window.Orientation.UNSPECIFIED;
  private originalLayoutFullScreen: boolean = false;
  private hasOriginalWindowLayout: boolean = false;
  private orientationRestored: boolean = false;
  private returnScheduled: boolean = false;
  private editorWindowRef: window.Window | null = null;
  private avoidAreaCallback: (data: window.AvoidAreaOptions) => void = (): void => {};

  aboutToAppear(): void {
    const params = (router.getParams() ?? {}) as Record<string, string>;
    this.profileId = params.profileId ?? 'default';
    this.profileName = params.profileName ?? '默认输入方案';
    this.appId = params.appId ?? '';
    this.baseProfileId = params.baseProfileId ?? 'default';
    this.returnAbilityName = params.returnAbilityName ?? '';
    this.returnSessionId = params.returnSessionId ?? '';
    this.returnToplevelId = params.returnToplevelId ?? '0';
    this.enterImmersive();
    InputProfileService.getInstance().initialize(this.context).then((): void => {
      this.profile = InputProfileService.getInstance().ensureProfile(this.profileId, this.profileName, this.baseProfileId);
      this.profileName = this.profile.name;
      this.previewRevision++;
    }).catch(() => {});
  }

  aboutToDisappear(): void {
    if (this.editorWindowRef) {
      try {
        this.editorWindowRef.off('avoidAreaChange', this.avoidAreaCallback);
      } catch (_) {
      }
      this.editorWindowRef = null;
    }
    if (!this.orientationRestored) this.restoreEditorWindow();
    this.resumeWineAbility();
  }

  /** 绘制编辑参考网格：20vp 细线 + 80vp 加粗线。Canvas 默认单位是 vp。 */
  private drawEditorGrid(): void {
    if (this.previewW <= 0 || this.previewH <= 0) {
      hilog.info(DOMAIN, 'InputLayoutEditor', 'drawEditorGrid skip w=%{public}d h=%{public}d',
        this.previewW, this.previewH);
      return;
    }
    const ctx = this.gridContext;
    const minor = 20;
    const major = 80;
    hilog.info(DOMAIN, 'InputLayoutEditor', 'drawEditorGrid w=%{public}d h=%{public}d',
      this.previewW, this.previewH);
    ctx.clearRect(0, 0, this.previewW, this.previewH);
    ctx.lineWidth = 1;
    for (let x = 0; x <= this.previewW; x += minor) {
      ctx.strokeStyle = x % major === 0 ? 'rgba(255,255,255,0.18)' : 'rgba(255,255,255,0.07)';
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, this.previewH);
      ctx.stroke();
    }
    for (let y = 0; y <= this.previewH; y += minor) {
      ctx.strokeStyle = y % major === 0 ? 'rgba(255,255,255,0.18)' : 'rgba(255,255,255,0.07)';
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(this.previewW, y + 0.5);
      ctx.stroke();
    }
  }

  private async enterImmersive(): Promise<void> {
    try {
      const editorWindow = await window.getLastWindow(this.context);
      this.originalOrientation = editorWindow.getPreferredOrientation();
      this.originalLayoutFullScreen = editorWindow.getWindowProperties().isLayoutFullScreen;
      this.hasOriginalWindowLayout = true;
      await editorWindow.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE_RESTRICTED);
      await editorWindow.setWindowLayoutFullScreen(true);
      try {
        await editorWindow.setSpecificSystemBarEnabled('status', false);
        await editorWindow.setSpecificSystemBarEnabled('navigationIndicator', false);
      } catch (_) {
        await editorWindow.setWindowSystemBarProperties({
          statusBarColor: '#00000000',
          navigationBarColor: '#00000000',
          isStatusBarLightIcon: true,
          isNavigationBarLightIcon: true
        });
      }
      this.refreshSafeAreas(editorWindow);
      this.editorWindowRef = editorWindow;
      this.avoidAreaCallback = (): void => {
        this.refreshSafeAreas(editorWindow);
      };
      try {
        editorWindow.on('avoidAreaChange', this.avoidAreaCallback);
      } catch (_) {
      }
      setTimeout((): void => { this.refreshSafeAreas(editorWindow); }, 180);
    } catch (_) {
    }
  }

  private refreshSafeAreas(editorWindow: window.Window): void {
    try {
      const systemAvoid = editorWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
      const cutoutAvoid = editorWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_CUTOUT);
      const navigationAvoid = editorWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);
      this.safeAreaTop = Math.max(systemAvoid.topRect.height, cutoutAvoid.topRect.height);
      this.safeAreaBottom = Math.max(systemAvoid.bottomRect.height, cutoutAvoid.bottomRect.height,
        navigationAvoid.bottomRect.height);
      this.safeAreaLeft = Math.max(systemAvoid.leftRect.width, cutoutAvoid.leftRect.width);
      this.safeAreaRight = Math.max(systemAvoid.rightRect.width, cutoutAvoid.rightRect.width);
    } catch (_) {
      this.safeAreaTop = 0;
      this.safeAreaBottom = 0;
      this.safeAreaLeft = 0;
      this.safeAreaRight = 0;
    }
  }

  private async restoreEditorWindow(): Promise<void> {
    if (this.orientationRestored) return;
    this.orientationRestored = true;
    try {
      const editorWindow = await window.getLastWindow(this.context);
      await editorWindow.setPreferredOrientation(this.originalOrientation);
      try {
        if (this.hasOriginalWindowLayout) {
          await editorWindow.setWindowLayoutFullScreen(this.originalLayoutFullScreen);
        }
      } catch (_) {
      }
      try {
        await editorWindow.setWindowSystemBarEnable(['status']);
      } catch (_) {
      }
      try {
        await editorWindow.setSpecificSystemBarEnabled('status', true);
      } catch (_) {
      }
      try {
        await editorWindow.setSpecificSystemBarEnabled('navigationIndicator', false);
      } catch (_) {
      }
      try {
        const settings = AppSettingsStore.getInstance().getGlobalSettings();
        let useLightIcons = settings.themeMode === ThemeMode.DARK;
        if (settings.themeMode === ThemeMode.SYSTEM) {
          useLightIcons = this.context.config.colorMode ===
            ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
        }
        await editorWindow.setWindowSystemBarProperties({
          statusBarColor: '#00000000',
          navigationBarColor: '#00000000',
          isStatusBarLightIcon: useLightIcons,
          isNavigationBarLightIcon: useLightIcons
        });
      } catch (_) {
      }
      this.safeAreaTop = 0;
      this.safeAreaBottom = 0;
      this.safeAreaLeft = 0;
      this.safeAreaRight = 0;
    } catch (_) {
    }
  }

  private async finishEditor(): Promise<void> {
    await this.restoreEditorWindow();
    router.back();
    this.resumeWineAbility();
  }

  private resumeWineAbility(): void {
    if (this.returnScheduled) return;
    if (this.returnAbilityName !== 'DesktopAbility') return;
    this.returnScheduled = true;
    const want: Want = {
      bundleName: 'com.vintage.pomelopro',
      abilityName: this.returnAbilityName,
      parameters: {
        sessionId: this.returnSessionId,
        toplevelId: Number(this.returnToplevelId) || 0
      }
    };
    setTimeout((): void => {
      this.context.startAbility(want).catch((): void => {});
    }, 80);
  }

  private selectedElement(): ControlElement | null {
    const elements = this.profile.elements;
    for (let index = 0; index < elements.length; index++) {
      if (elements[index].id === this.selectedElementId) return elements[index];
    }
    return null;
  }

  private updateProfile(mutator: (elements: ControlElement[]) => ControlElement[]): void {
    const next = cloneInputProfile(this.profile);
    next.elements = mutator(next.elements ?? []);
    this.profile = next;
    this.previewRevision++;
  }

  private moveElement(id: string, x: number, y: number): void {
    this.updateProfile((elements: ControlElement[]): ControlElement[] =>
      elements.map((element: ControlElement): ControlElement =>
        element.id === id ? this.withElement(element, { x: x, y: y }) : element));
  }

  private scaleElement(id: string, scale: number): void {
    this.updateProfile((elements: ControlElement[]): ControlElement[] =>
      elements.map((element: ControlElement): ControlElement =>
        element.id === id ? this.withElement(element, { scale: scale }) : element));
  }

  private updateSelectedElement(mutator: (element: ControlElement) => ControlElement): void {
    const id = this.selectedElementId;
    this.updateProfile((elements: ControlElement[]): ControlElement[] =>
      elements.map((element: ControlElement): ControlElement =>
        element.id === id ? mutator(element) : element));
  }

  private withElement(element: ControlElement, patch: ElementPatch): ControlElement {
    const next = cloneControlElement(element);
    if (patch.x !== undefined) next.x = patch.x;
    if (patch.y !== undefined) next.y = patch.y;
    if (patch.scale !== undefined) next.scale = patch.scale;
    if (patch.widthScale !== undefined) next.widthScale = patch.widthScale;
    if (patch.text !== undefined) next.text = patch.text;
    if (patch.shape !== undefined) next.shape = patch.shape;
    if (patch.toggleSwitch !== undefined) next.toggleSwitch = patch.toggleSwitch;
    if (patch.opacity !== undefined) next.opacity = patch.opacity;
    if (patch.bindings !== undefined) next.bindings = patch.bindings;
    if (patch.keyList !== undefined) next.keyList = patch.keyList;
    if (patch.orientation !== undefined) next.orientation = patch.orientation;
    if (patch.outputMode !== undefined) next.outputMode = patch.outputMode;
    return next;
  }

  private elementPixelWidth(element: ControlElement): number {
    if (element.type === ControlElementType.PANEL) {
      return this.previewW * Math.max(0.1, Math.min(1, element.panelWidthRatio ?? 0.84));
    }
    const viewportScale = this.previewControlScale();
    const widthScale = element.widthScale ?? 1;
    const keyWidthRatio = element.keyWidthRatio;
    if (keyWidthRatio !== undefined && keyWidthRatio > 0) {
      return this.previewW * keyWidthRatio * (element.scale ?? 1) * widthScale;
    }
    if (element.type === ControlElementType.TRACKPAD) {
      return 150 * (element.scale ?? 1) * widthScale * viewportScale;
    }
    if (element.type === ControlElementType.RANGE_BUTTON) {
      return (element.orientation === 'horizontal' ? 180 : 44) *
        (element.scale ?? 1) * widthScale * viewportScale;
    }
    if (element.type === ControlElementType.D_PAD) return 132 * (element.scale ?? 1) * widthScale * viewportScale;
    if (element.type === ControlElementType.STICK) return 120 * (element.scale ?? 1) * widthScale * viewportScale;
    return 56 * (element.scale ?? 1) * widthScale * viewportScale;
  }

  private elementPixelHeight(element: ControlElement): number {
    if (element.type === ControlElementType.PANEL) {
      return this.previewH * Math.max(0.1, Math.min(1, element.panelHeightRatio ?? 0.48));
    }
    const viewportScale = this.previewControlScale();
    const heightScale = element.heightScale ?? 1;
    const keyHeightRatio = element.keyHeightRatio;
    if (keyHeightRatio !== undefined && keyHeightRatio > 0) {
      return this.previewH * keyHeightRatio * (element.scale ?? 1) * heightScale;
    }
    if (element.type === ControlElementType.TRACKPAD) {
      return 100 * (element.scale ?? 1) * heightScale * viewportScale;
    }
    if (element.type === ControlElementType.RANGE_BUTTON) {
      return (element.orientation === 'horizontal' ? 44 : 180) *
        (element.scale ?? 1) * heightScale * viewportScale;
    }
    if (element.type === ControlElementType.D_PAD) {
      return 132 * (element.scale ?? 1) * heightScale * viewportScale;
    }
    if (element.type === ControlElementType.STICK) {
      return 120 * (element.scale ?? 1) * heightScale * viewportScale;
    }
    return 56 * (element.scale ?? 1) * heightScale * viewportScale;
  }

  private previewControlScale(): number {
    if (this.previewW <= 0 || this.previewH <= 0) return 1;
    const longSide = Math.max(this.previewW, this.previewH);
    const shortSide = Math.min(this.previewW, this.previewH);
    return Math.max(0.85, Math.min(1.65, Math.min(longSide / 809, shortSide / 365)));
  }

  private elementsOverlap(left: ControlElement, right: ControlElement): boolean {
    const halfWidth = (this.elementPixelWidth(left) + this.elementPixelWidth(right)) / 2;
    const halfHeight = (this.elementPixelHeight(left) + this.elementPixelHeight(right)) / 2;
    const dx = Math.abs(left.x - right.x) * this.previewW;
    const dy = Math.abs(left.y - right.y) * this.previewH;
    return dx < halfWidth && dy < halfHeight;
  }

  private overlapWarning(): string {
    const element = this.selectedElement();
    if (!element) return '';
    const elements = this.profile.elements;
    for (let index = 0; index < elements.length; index++) {
      const other = elements[index];
      if (other.id === element.id || !other.visible) continue;
      if (this.elementsOverlap(element, other)) {
        const label = other.text.length > 0 ? other.text : '未命名控件';
        return '与「' + label + '」重叠，请拖动错开';
      }
    }
    return '';
  }

  private panelPositionX(): number {
    return this.panelX >= 0 ? this.panelX : Math.max(8, this.previewW - 288);
  }

  private panelPositionY(): number {
    return this.panelY >= 0 ? this.panelY : 64;
  }

  private beginPanelDrag(): void {
    this.panelX = this.panelPositionX();
    this.panelY = this.panelPositionY();
    this.panelDragStartX = this.panelX;
    this.panelDragStartY = this.panelY;
  }

  private movePanel(offsetX: number, offsetY: number): void {
    const maxX = Math.max(8, this.previewW - 288);
    const maxY = Math.max(8, this.previewH - 120);
    this.panelX = Math.max(8, Math.min(maxX, this.panelDragStartX + offsetX));
    this.panelY = Math.max(8, Math.min(maxY, this.panelDragStartY + offsetY));
  }

  private handlePanelHeaderTouch(event: TouchEvent): void {
    const touch = event.changedTouches[0];
    if (!touch) return;
    if (event.type === TouchType.Down) {
      this.panelDragTouchId = touch.id;
      this.panelTouchStartX = Number(touch.x);
      this.panelTouchStartY = Number(touch.y);
      this.beginPanelDrag();
      return;
    }
    if (event.type === TouchType.Move && this.panelDragTouchId === touch.id) {
      this.movePanel(Number(touch.x) - this.panelTouchStartX,
        Number(touch.y) - this.panelTouchStartY);
      return;
    }
    if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      if (this.panelDragTouchId === touch.id) this.panelDragTouchId = -1;
    }
  }

  private toolbarPositionX(): number {
    if (this.toolbarX >= 0) return this.toolbarX;
    return Math.max(8, (this.previewW - 520) / 2);
  }

  private toolbarPositionY(): number {
    return this.toolbarY >= 0 ? this.toolbarY : 16;
  }

  private beginToolbarDrag(): void {
    this.toolbarX = this.toolbarPositionX();
    this.toolbarY = this.toolbarPositionY();
    this.toolbarDragStartX = this.toolbarX;
    this.toolbarDragStartY = this.toolbarY;
  }

  private moveToolbar(offsetX: number, offsetY: number): void {
    const maxX = Math.max(8, this.previewW - 740);
    const maxY = Math.max(8, this.previewH - 140);
    this.toolbarX = Math.max(8, Math.min(maxX, this.toolbarDragStartX + offsetX));
    this.toolbarY = Math.max(8, Math.min(maxY, this.toolbarDragStartY + offsetY));
  }

  private handleToolbarHandleTouch(event: TouchEvent): void {
    const touch = event.changedTouches[0];
    if (!touch) return;
    if (event.type === TouchType.Down) {
      this.toolbarDragTouchId = touch.id;
      this.toolbarTouchStartX = Number(touch.x);
      this.toolbarTouchStartY = Number(touch.y);
      this.beginToolbarDrag();
      return;
    }
    if (event.type === TouchType.Move && this.toolbarDragTouchId === touch.id) {
      this.moveToolbar(Number(touch.x) - this.toolbarTouchStartX,
        Number(touch.y) - this.toolbarTouchStartY);
      return;
    }
    if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      if (this.toolbarDragTouchId === touch.id) this.toolbarDragTouchId = -1;
    }
  }

  private defaultBindings(type: ControlElementType): InputBinding[] {
    if (type === ControlElementType.D_PAD || type === ControlElementType.STICK) {
      return [keyBinding(17), keyBinding(32), keyBinding(31), keyBinding(30)];
    }
    if (type === ControlElementType.BUTTON) return [keyBinding(57)];
    return [];
  }

  private addElement(type: ControlElementType): void {
    const keyList = type === ControlElementType.RANGE_BUTTON ? [2, 3, 4, 5, 6] : [];
    const element = createControlElement(type, 0.5, 0.5, elementTypeLabel(type), this.defaultBindings(type),
      undefined, 1, false, keyList);
    this.updateProfile((elements: ControlElement[]): ControlElement[] => {
      const next = elements.slice();
      next.push(element);
      return next;
    });
    this.selectedElementId = element.id;
    this.showPropertyPanel = true;
  }

  private removeSelectedElement(): void {
    const id = this.selectedElementId;
    if (!id) return;
    this.updateProfile((elements: ControlElement[]): ControlElement[] =>
      elements.filter((element: ControlElement): boolean => element.id !== id));
    this.selectedElementId = '';
    this.showPropertyPanel = false;
  }

  private duplicateSelectedElement(): void {
    const source = this.selectedElement();
    if (!source) return;
    const copy = createControlElement(source.type, Math.min(0.95, source.x + 0.04), Math.min(0.95, source.y + 0.04),
      source.text, source.bindings, source.shape, source.scale, source.toggleSwitch, source.keyList,
      source.orientation, source.outputMode, source.widthScale, source.heightScale);
    copy.panelWidthRatio = source.panelWidthRatio;
    copy.panelHeightRatio = source.panelHeightRatio;
    copy.opacity = source.opacity;
    this.updateProfile((elements: ControlElement[]): ControlElement[] => {
      const next = elements.slice();
      next.push(copy);
      return next;
    });
    this.selectedElementId = copy.id;
  }

  private reset(): void {
    // 恢复“本次编辑前”的状态: 重新读取已保存的当前方案, 不重置为出厂模板。
    let saved = InputProfileService.getInstance().getProfile(this.profileId);
    if (saved.id !== this.profileId) {
      saved = InputProfileService.getInstance().getProfile(this.baseProfileId);
    }
    this.profile = cloneInputProfile(saved);
    this.profile.id = this.profileId;
    this.profile.name = this.profileName;
    this.selectedElementId = '';
    this.showPropertyPanel = false;
    this.previewRevision++;
    promptAction.showToast({ message: '已恢复本次修改前的方案' });
  }

  private async save(): Promise<void> {
    const saved = cloneInputProfile(this.profile);
    saved.id = this.profileId;
    saved.name = this.profileName;
    await InputProfileService.getInstance().saveProfile(saved);
    if (this.appId) {
      const current = cloneAppSettings(AppSettingsStore.getInstance().getAppSettings(this.appId));
      current.inputProfileId = this.profile.id;
      await AppSettingsStore.getInstance().saveAppSettings(current);
    }
    promptAction.showToast({ message: '输入方案已保存' });
    await this.finishEditor();
  }

  private setBindingSlot(slotIndex: number, binding: InputBinding): void {
    this.updateSelectedElement((element: ControlElement): ControlElement => {
      const bindings = element.bindings.slice();
      while (bindings.length <= slotIndex) {
        bindings.push({ kind: InputBindingKind.NONE, code: 0 });
      }
      bindings[slotIndex] = binding;
      return this.withElement(element, { bindings: bindings });
    });
  }

  private addRangeKey(code: number): void {
    this.updateSelectedElement((element: ControlElement): ControlElement => {
      const keyList = element.keyList ?? [];
      if (keyList.indexOf(code) >= 0 || keyList.length >= 8) return element;
      const next = keyList.slice();
      next.push(code);
      return this.withElement(element, {
        keyList: next,
        bindings: next.length > 0 ? [keyBinding(next[0])] : []
      });
    });
  }

  private removeRangeKey(code: number): void {
    this.updateSelectedElement((element: ControlElement): ControlElement => {
      const keyList = (element.keyList ?? []).filter((value: number): boolean => value !== code);
      return this.withElement(element, {
        keyList: keyList,
        bindings: keyList.length > 0 ? [keyBinding(keyList[0])] : []
      });
    });
  }

  private pickerOptions(): PickerOption[] {
    const options: PickerOption[] = [
      { label: '无绑定', name: 'NONE', kind: InputBindingKind.NONE, code: 0 },
      { label: '鼠标左键', name: 'MOUSE_LEFT_BUTTON', kind: InputBindingKind.MOUSE, code: MOUSE_LEFT_BUTTON },
      { label: '鼠标右键', name: 'MOUSE_RIGHT_BUTTON', kind: InputBindingKind.MOUSE, code: MOUSE_RIGHT_BUTTON },
      { label: '鼠标中键', name: 'MOUSE_MIDDLE_BUTTON', kind: InputBindingKind.MOUSE, code: MOUSE_MIDDLE_BUTTON }
    ];
    for (let index = 0; index < KEY_NAME_OPTIONS.length; index++) {
      const option = KEY_NAME_OPTIONS[index];
      options.push({ label: option.label, name: option.name, kind: InputBindingKind.KEY, code: option.code });
    }
    return options;
  }

  private applyPickerSelection(option: PickerOption): void {
    if (this.pickerTargetKeyList && option.kind === InputBindingKind.KEY && option.code > 0) {
      this.addRangeKey(option.code);
    } else if (this.pickerTargetSlot >= 0) {
      this.setBindingSlot(this.pickerTargetSlot, {
        kind: option.kind, code: option.code
      });
    }
    this.pickerOpen = false;
    this.pickerTargetSlot = -1;
    this.pickerTargetKeyList = false;
  }

  private applyCustomKey(): void {
    const parsed = Number(this.customKeyText.trim());
    if (!parsed || Number.isNaN(parsed) || parsed <= 0) {
      promptAction.showToast({ message: '请输入有效的键码（如 57=空格、17=W）' });
      return;
    }
    if (this.pickerTargetSlot >= 0) {
      this.setBindingSlot(this.pickerTargetSlot, keyBinding(Math.round(parsed)));
      this.pickerOpen = false;
      this.pickerTargetSlot = -1;
    } else if (this.pickerTargetKeyList) {
      this.addRangeKey(Math.round(parsed));
      this.pickerOpen = false;
      this.pickerTargetKeyList = false;
    }
    this.customKeyText = '';
  }

  private openSlotPicker(slotIndex: number): void {
    this.pickerTargetSlot = slotIndex;
    this.pickerTargetKeyList = false;
    this.pickerOpen = true;
  }

  private openKeyListPicker(): void {
    this.pickerTargetSlot = -1;
    this.pickerTargetKeyList = true;
    this.pickerOpen = true;
  }

  private bindingLabel(binding: InputBinding | undefined): string {
    if (!binding || binding.kind === InputBindingKind.NONE || binding.code <= 0) return '未绑定';
    if (binding.kind === InputBindingKind.MOUSE) {
      if (binding.code === MOUSE_LEFT_BUTTON) return '鼠标左键';
      if (binding.code === MOUSE_RIGHT_BUTTON) return '鼠标右键';
      if (binding.code === MOUSE_MIDDLE_BUTTON) return '鼠标中键';
      return '鼠标 ' + binding.code.toString();
    }
    return keyNameLabel(binding.code);
  }

  // ---- 编辑器 UI ----

  @Builder
  private AddChip(label: string, accent: string, action: () => void, id: string) {
    Text(label).fontSize(11).fontColor(accent).padding({ left: 9, right: 9, top: 5, bottom: 5 })
      .backgroundColor('#E8262626').borderRadius(13).border({ width: 1, color: '#33FFFFFF' })
      .id(id).hitTestBehavior(HitTestMode.Block).onClick(action)
  }

  @Builder
  private ToolbarIconButton(icon: string, accent: string, action: () => void, id: string, size: number = 30) {
    Text(icon).fontSize(14).fontColor(accent).width(size).height(size).textAlign(TextAlign.Center)
      .backgroundColor('#E8262626').borderRadius(size / 2).border({ width: 1, color: '#33FFFFFF' })
      .id(id).hitTestBehavior(HitTestMode.Block).onClick(action)
  }

  @Builder
  private ToolbarContainer() {
    Row({ space: 6 }) {
      Text('≡').fontSize(15).fontColor('#FFFFFFAA').width(28).height(28).textAlign(TextAlign.Center)
        .backgroundColor('#33262626').borderRadius(14)
        .id('editor-toolbar-handle')
        .hitTestBehavior(HitTestMode.Block)
        .onTouch((event: TouchEvent): void => this.handleToolbarHandleTouch(event))
      Row().width(1).height(24).backgroundColor('#33FFFFFF')
      this.AddChip('键', this.editorAccent(), (): void => this.addElement(ControlElementType.BUTTON), 'editor-add-button')
      this.AddChip('方向', this.editorChipColor('#FF4CAF50'), (): void => this.addElement(ControlElementType.D_PAD), 'editor-add-dpad')
      this.AddChip('摇杆', this.editorChipColor('#FF8BC34A'), (): void => this.addElement(ControlElementType.STICK), 'editor-add-stick')
      this.AddChip('触摸板', this.editorChipColor('#FFFFB74D'), (): void => this.addElement(ControlElementType.TRACKPAD), 'editor-add-trackpad')
      this.AddChip('滚动键', this.editorChipColor('#FFAB47BC'), (): void => this.addElement(ControlElementType.RANGE_BUTTON), 'editor-add-range')
      Row().width(1).height(24).backgroundColor('#33FFFFFF')
      this.ToolbarIconButton('✕', '#FFFF5252', (): void => { this.finishEditor(); }, 'input-layout-back')
      this.ToolbarIconButton('↺', '#FFFFB74D', (): void => { this.reset(); }, 'input-layout-reset')
      this.ToolbarIconButton('⧉', '#FFAB47BC', (): void => { this.duplicateSelectedElement(); }, 'input-layout-duplicate')
      this.ToolbarIconButton('删', '#FFFF5252', (): void => { this.removeSelectedElement(); }, 'input-layout-delete')
      this.ToolbarIconButton('✓', '#FFFFFFFF', (): void => { this.save(); }, 'input-layout-save', 32)
    }
    .padding({ left: 8, right: 8, top: 5, bottom: 5 })
    .backgroundColor('#D914141C').borderRadius(20).border({ width: 1, color: '#44FFFFFF' })
    .position({ x: this.toolbarPositionX(), y: this.toolbarPositionY() })
    .zIndex(130).id('editor-toolbar')
  }

  @Builder
  private ShapeChip(label: string, shape: ControlElementShape) {
    Text(label).fontSize(11).fontColor(this.selectedElement()?.shape === shape ? '#FFFFFFFF' : '#FFFFFF99')
      .padding({ left: 9, right: 9, top: 6, bottom: 6 })
      .backgroundColor(this.selectedElement()?.shape === shape ? this.editorSelectedBackground() : '#18FFFFFF')
      .borderRadius(14).border({ width: 1, color: this.selectedElement()?.shape === shape ? this.editorAccent() : '#33FFFFFF' })
      .onClick((): void => {
        this.updateSelectedElement((element: ControlElement): ControlElement => this.withElement(element, { shape: shape }));
      })
  }

  @Builder
  private BindingSlots() {
    if (this.selectedElementId.length > 0) {
      if (this.selectedElement()?.type === ControlElementType.D_PAD ||
        this.selectedElement()?.type === ControlElementType.STICK) {
        Column({ space: 6 }) {
          if (this.selectedElement()?.type === ControlElementType.STICK) {
            Row({ space: 8 }) {
              Text('输出').fontSize(11).fontColor('#FFFFFFAA')
              Text('按键').fontSize(11)
                .fontColor((this.selectedElement()?.outputMode ?? 'keys') === 'keys' ? this.editorAccent() : '#FFFFFF88')
                .padding({ left: 10, right: 10, top: 6, bottom: 6 })
                .backgroundColor((this.selectedElement()?.outputMode ?? 'keys') === 'keys' ? this.editorSelectedBackground() : '#18FFFFFF')
                .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
                .onClick((): void => {
                  this.updateSelectedElement((element: ControlElement): ControlElement =>
                    this.withElement(element, { outputMode: 'keys' }));
                })
              Text('鼠标视角').fontSize(11)
                .fontColor(this.selectedElement()?.outputMode === 'mouse' ? this.editorAccent() : '#FFFFFF88')
                .padding({ left: 10, right: 10, top: 6, bottom: 6 })
                .backgroundColor(this.selectedElement()?.outputMode === 'mouse' ? this.editorSelectedBackground() : '#18FFFFFF')
                .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
                .onClick((): void => {
                  this.updateSelectedElement((element: ControlElement): ControlElement =>
                    this.withElement(element, { outputMode: 'mouse' }));
                })
            }.width('100%')
          }
          Text('方向绑定（上/右/下/左）').fontSize(11).fontColor('#FFFFFFAA').width('100%')
          ForEach([0, 1, 2, 3], (slot: number): void => {
            Row({ space: 8 }) {
              Text(['上', '右', '下', '左'][slot]).fontSize(11).fontColor('#FFFFFF88').width(24)
              Text(this.bindingLabel(this.selectedElement()?.bindings[slot])).fontSize(12)
                .fontColor(this.editorBindingText())
                .layoutWeight(1).textAlign(TextAlign.Center)
                .padding({ top: 7, bottom: 7 }).backgroundColor(this.editorSelectedBackground())
                .borderRadius(10).border({ width: 1, color: '#55FFFFFF' })
                .onClick((): void => this.openSlotPicker(slot))
            }.width('100%')
          }, (slot: number): string => slot.toString())
        }.width('100%')
      } else if (this.selectedElement()?.type === ControlElementType.BUTTON) {
        Column({ space: 6 }) {
          Text('按键绑定（最多 4 个，同时按下）').fontSize(11).fontColor('#FFFFFFAA').width('100%')
          ForEach([0, 1, 2, 3], (slot: number): void => {
            Row({ space: 8 }) {
              Text(this.bindingLabel(this.selectedElement()?.bindings[slot])).fontSize(12)
                .fontColor(this.editorBindingText())
                .layoutWeight(1).textAlign(TextAlign.Center)
                .padding({ top: 7, bottom: 7 }).backgroundColor(this.editorSelectedBackground())
                .borderRadius(10).border({ width: 1, color: '#55FFFFFF' })
                .onClick((): void => this.openSlotPicker(slot))
            }.width('100%')
          }, (slot: number): string => slot.toString())
        }.width('100%')
      } else if (this.selectedElement()?.type === ControlElementType.RANGE_BUTTON) {
        Column({ space: 6 }) {
          Row({ space: 8 }) {
            Text('方向').fontSize(11).fontColor('#FFFFFFAA')
            Text('纵向').fontSize(11)
              .fontColor((this.selectedElement()?.orientation ?? 'vertical') === 'vertical' ? this.editorAccent() : '#FFFFFF88')
              .padding({ left: 10, right: 10, top: 6, bottom: 6 })
              .backgroundColor((this.selectedElement()?.orientation ?? 'vertical') === 'vertical' ? this.editorSelectedBackground() : '#18FFFFFF')
              .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
              .onClick((): void => {
                this.updateSelectedElement((element: ControlElement): ControlElement =>
                  this.withElement(element, { orientation: 'vertical' }));
              })
            Text('横向').fontSize(11)
              .fontColor(this.selectedElement()?.orientation === 'horizontal' ? this.editorAccent() : '#FFFFFF88')
              .padding({ left: 10, right: 10, top: 6, bottom: 6 })
              .backgroundColor(this.selectedElement()?.orientation === 'horizontal' ? this.editorSelectedBackground() : '#18FFFFFF')
              .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
              .onClick((): void => {
                this.updateSelectedElement((element: ControlElement): ControlElement =>
                  this.withElement(element, { orientation: 'horizontal' }));
              })
          }.width('100%')
          Text('可选键列表（滑动选键，点按发送）').fontSize(11).fontColor('#FFFFFFAA').width('100%')
          Flex({ wrap: FlexWrap.Wrap }) {
            ForEach(this.selectedElement()?.keyList ?? [], (code: number): void => {
              Text(keyNameLabel(code) + ' ✕').fontSize(11).fontColor('#FFFFFFFF')
                .padding({ left: 9, right: 9, top: 6, bottom: 6 }).margin({ right: 6, bottom: 6 })
                .backgroundColor('#33404040').borderRadius(12).border({ width: 1, color: '#55FFFFFF' })
                .onClick((): void => this.removeRangeKey(code))
            }, (code: number): string => code.toString())
          }.width('100%')
          Text('＋ 添加键').fontSize(11).fontColor(this.editorAccent())
            .padding({ left: 9, right: 9, top: 6, bottom: 6 })
            .backgroundColor('#18FFFFFF').borderRadius(12).border({ width: 1, color: '#55FFFFFF' })
            .onClick((): void => this.openKeyListPicker())
        }.width('100%')
      } else if (this.selectedElement()?.type === ControlElementType.TRACKPAD) {
        Text('触摸板不绑定按键：按住滑动移动鼠标，轻点发送左键。')
          .fontSize(11).fontColor('#FFFFFF88').width('100%')
      }
    }
  }

  @Builder
  private PropertyPanel() {
    if (this.selectedElementId.length > 0) {
      Column() {
        Row() {
          Row({ space: 7 }) {
            Text('▣').fontSize(12).fontColor(this.editorAccent()).fontWeight(FontWeight.Bold)
            Text('元素设置 · ' + elementTypeLabel(this.selectedElement()?.type ?? ControlElementType.BUTTON)).fontSize(13)
              .fontColor('#FFFFFFFF').fontWeight(FontWeight.Medium)
          }
          .layoutWeight(1)
        }
        .width('100%').height(34).padding({ left: 12, right: 12 })
        .hitTestBehavior(HitTestMode.Block)
        .onTouch((event: TouchEvent): void => this.handlePanelHeaderTouch(event))
        Scroll() {
          Column({ space: 10 }) {
          if (this.overlapWarning().length > 0) {
            Text(this.overlapWarning()).fontSize(11).fontColor('#FFFF5252').width('100%')
          }
          Text('文字标签').fontSize(11).fontColor('#FFFFFFAA').width('100%')
          TextInput({ text: this.selectedElement()?.text ?? '', placeholder: '显示文字（可留空）' })
            .height(36).fontSize(12).fontColor('#FFFFFFFF')
            .backgroundColor('#22FFFFFF').borderRadius(8)
            .maxLength(12)
            .onChange((value: string): void => {
              this.updateSelectedElement((current: ControlElement): ControlElement => this.withElement(current, { text: value }));
            })
          Text('形状').fontSize(11).fontColor('#FFFFFFAA').width('100%')
          Row({ space: 8 }) {
            this.ShapeChip('圆形', ControlElementShape.CIRCLE)
            this.ShapeChip('方形', ControlElementShape.SQUARE)
            this.ShapeChip('圆角矩形', ControlElementShape.ROUND_RECT)
            this.ShapeChip('矩形', ControlElementShape.RECT)
          }.width('100%')
          if (this.selectedElement()?.type === ControlElementType.BUTTON) {
            Row({ space: 8 }) {
              Text('按住锁定（再按释放）').fontSize(11).fontColor('#FFFFFFAA')
              Text(this.selectedElement()?.toggleSwitch ? '开' : '关').fontSize(11)
                .fontColor(this.selectedElement()?.toggleSwitch ? this.editorAccent() : '#FFFFFF88')
                .padding({ left: 9, right: 9, top: 6, bottom: 6 })
                .backgroundColor(this.selectedElement()?.toggleSwitch ? this.editorSelectedBackground() : '#18FFFFFF')
                .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
                .onClick((): void => {
                  this.updateSelectedElement((current: ControlElement): ControlElement =>
                    this.withElement(current, { toggleSwitch: !current.toggleSwitch }));
                })
            }.width('100%')
          }
          Text('缩放 ' + Math.round((this.selectedElement()?.scale ?? 1) * 100) + '%')
            .fontSize(11).fontColor('#FFFFFFAA').width('100%')
          Slider({ value: (this.selectedElement()?.scale ?? 1) * 100, min: 10, max: 500, step: 5, style: SliderStyle.OutSet })
            .width('100%').blockColor(this.editorAccent()).selectedColor(this.editorAccent()).trackColor('#FFFFFF22')
            .onChange((value: number, _mode: SliderChangeMode): void => {
              this.scaleElement(this.selectedElementId, value / 100);
            })
          if (this.selectedElement()?.type === ControlElementType.BUTTON) {
            Text('按键宽度 ' + Math.round((this.selectedElement()?.widthScale ?? 1) * 100) + '%')
              .fontSize(11).fontColor('#FFFFFFAA').width('100%')
            Slider({ value: (this.selectedElement()?.widthScale ?? 1) * 100,
              min: 20, max: 1200, step: 10, style: SliderStyle.OutSet })
              .width('100%').blockColor(this.editorAccent()).selectedColor(this.editorAccent()).trackColor('#FFFFFF22')
              .onChange((value: number, _mode: SliderChangeMode): void => {
                this.updateSelectedElement((current: ControlElement): ControlElement =>
                  this.withElement(current, { widthScale: value / 100 }));
              })
          }
          Row({ space: 8 }) {
            Text('透明度 ' + ((this.selectedElement()?.opacity ?? -1) >= 0 ?
              Math.round(this.selectedElement()?.opacity ?? 70) : 100) + '%')
              .fontSize(11).fontColor('#FFFFFFAA')
            Text((this.selectedElement()?.opacity ?? -1) >= 0 ? '自定义' : '继承方案')
              .fontSize(11).fontColor((this.selectedElement()?.opacity ?? -1) >= 0 ? this.editorAccent() : '#FFFFFF88')
              .padding({ left: 9, right: 9, top: 6, bottom: 6 })
              .backgroundColor((this.selectedElement()?.opacity ?? -1) >= 0 ? this.editorSelectedBackground() : '#18FFFFFF')
              .borderRadius(12).border({ width: 1, color: '#44FFFFFF' })
              .onClick((): void => {
                this.updateSelectedElement((current: ControlElement): ControlElement =>
                  this.withElement(current, { opacity: current.opacity >= 0 ? -1 : 70 }));
              })
          }.width('100%')
          if ((this.selectedElement()?.opacity ?? -1) >= 0) {
            Slider({ value: this.selectedElement()?.opacity ?? 70, min: 10, max: 100, step: 5, style: SliderStyle.OutSet })
              .width('100%').blockColor('#FFAB47BC').selectedColor('#FFAB47BC').trackColor('#FFFFFF22')
              .onChange((value: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.updateSelectedElement((current: ControlElement): ControlElement =>
                    this.withElement(current, { opacity: value }));
                }
              })
          }
          this.BindingSlots()
          Text('提示：拖动元素调整位置，双指捏合缩放；删除/复制按钮在下方操作栏。')
            .fontSize(10).fontColor('#FFFFFF88').width('100%')
          }
          .width('100%').padding(12)
        }
        .constraintSize({ maxHeight: Math.max(200, this.previewH - 200) })
        .scrollBar(BarState.Off)
        .edgeEffect(EdgeEffect.Spring)
      }
      .width(280)
      .backgroundColor('#EE111111').borderRadius(14).border({ width: 1, color: '#44FFFFFF' })
      .position({ x: this.panelPositionX(), y: this.panelPositionY() }).zIndex(100).id('vintage-layout-config')
    }
  }

  @Builder
  private KeyPicker() {
    Column({ space: 10 }) {
      Row() {
        Text(this.pickerTargetKeyList ? '添加可选键' : '选择绑定键').fontSize(14)
          .fontColor('#FFFFFFFF').fontWeight(FontWeight.Medium).layoutWeight(1)
        Text('✕').fontSize(16).fontColor('#FFFF5252').onClick((): void => {
          this.pickerOpen = false;
          this.pickerTargetSlot = -1;
          this.pickerTargetKeyList = false;
        })
      }.width('100%')
      Scroll() {
        Column({ space: 6 }) {
          ForEach(this.pickerOptions(), (option: PickerOption): void => {
            Row({ space: 10 }) {
              Text(option.label).fontSize(13).fontColor('#FFFFFFFF').layoutWeight(1)
              Text(option.name).fontSize(10).fontColor('#FFFFFF77')
            }
            .width('100%').padding({ left: 12, right: 12, top: 9, bottom: 9 })
            .backgroundColor('#22FFFFFF').borderRadius(10)
            .onClick((): void => this.applyPickerSelection(option))
          }, (option: PickerOption): string => option.kind + ':' + option.code.toString())
        }.width('100%')
      }
      .constraintSize({ maxHeight: this.previewH * 0.52 })
      .scrollBar(BarState.Off)
      Row({ space: 8 }) {
        TextInput({ text: this.customKeyText, placeholder: '自定义键码（如 57）' })
          .height(36).layoutWeight(1).fontSize(12).fontColor('#FFFFFFFF')
          .backgroundColor('#22FFFFFF').borderRadius(8)
          .type(InputType.Number)
          .onChange((value: string): void => { this.customKeyText = value; })
        Text('应用').fontSize(12).fontColor(this.editorAccent()).padding({ left: 12, right: 12, top: 8, bottom: 8 })
          .backgroundColor(this.editorSelectedBackground()).borderRadius(12)
          .onClick((): void => this.applyCustomKey())
      }.width('100%')
    }
    .width(320).padding(16)
    .backgroundColor('#EE15151F').borderRadius(16).border({ width: 1, color: '#55FFFFFF' })
    .position({ x: '50%', y: '50%' }).translate({ x: '-50%', y: '-50%' }).zIndex(200)
  }

  build() {
    Column() {
      if (this.safeAreaTop > 0) {
        Row().width('100%').height(this.safeAreaTop + 'px').backgroundColor('#FF000000')
      }
      Stack() {
        Column().width('100%').height('100%').backgroundColor('#FF1A1A2E')
        // 编辑参考网格：密集细线，帮助按钮对齐（只在编辑器内显示）。
        Canvas(this.gridContext)
          .width('100%').height('100%')
          .onReady(() => this.drawEditorGrid())
        .width('100%').height('100%')
        .hitTestBehavior(HitTestMode.None)
        VirtualInputOverlay({
          profile: this.profile,
          renderRevision: this.previewRevision,
          interactive: false,
          hideToolbar: true,
          editMode: true,
          selectedElementId: this.selectedElementId,
          onElementMove: (id: string, x: number, y: number): void => this.moveElement(id, x, y),
          onElementScale: (id: string, scale: number): void => this.scaleElement(id, scale),
          onElementSelected: (id: string): void => {
            this.selectedElementId = id;
            this.showPropertyPanel = id.length > 0;
          }
        })
          .width('100%').height('100%')
        this.ToolbarContainer()
        if (this.showPropertyPanel && this.selectedElementId.length > 0) {
          this.PropertyPanel()
        }
        if (this.pickerOpen) {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => {
              this.pickerOpen = false;
              this.pickerTargetSlot = -1;
              this.pickerTargetKeyList = false;
            })
          this.KeyPicker()
        }
      }
      .width('100%').height('100%')
      .onAreaChange((_oldValue: Area, area: Area): void => {
        this.previewW = area.width as number;
        this.previewH = area.height as number;
        this.drawEditorGrid();
      })
    }
    .width('100%').height('100%').backgroundColor('#FF1A1A2E')
  }
}
