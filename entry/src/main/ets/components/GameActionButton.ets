import { InputEventGate } from '../service/InputEventGate';
import { DECK_COLORS, skinColor } from '../service/SkinColors';

// Input-overlay subwindows use the same explicit touch/mouse path as VPP's
// toolbar. A synthetic click must not dispatch the action a second time.
@Component
export struct GameActionButton {
  @Prop label: string = '';
  @Prop selected: boolean = false;
  @Prop fullWidth: boolean = false;
  @Prop controlId: string = '';
  @Prop darkPanel: boolean = false;
  @Prop disabled: boolean = false;
  onAction: () => void = (): void => {};
  private gate: InputEventGate = new InputEventGate();
  private lastAction: number = 0;
  private invoke(): void {
    if (this.disabled) return;
    const now = Date.now();
    if (now - this.lastAction < 250) return;
    this.lastAction = now; this.onAction();
  }
  build() {
    Button(this.label).height(38).fontSize(13).width(this.fullWidth ? '100%' : undefined)
      .borderRadius(12)
      .fontColor(this.selected ? (this.darkPanel ? DECK_COLORS.accent : skinColor('accent')) :
        this.darkPanel ? DECK_COLORS.text_primary : skinColor('text_primary'))
      .backgroundColor(this.selected ? (this.darkPanel ? DECK_COLORS.selected_background : skinColor('selected_background')) :
        this.darkPanel ? DECK_COLORS.card_background : skinColor('search_background'))
      .border({ width: 1, color: this.selected ? (this.darkPanel ? DECK_COLORS.accent : skinColor('accent')) :
        this.darkPanel ? DECK_COLORS.card_border : skinColor('card_border') })
      .enabled(!this.disabled)
      .opacity(this.disabled ? 0.45 : 1)
      .hitTestBehavior(HitTestMode.Block).id(this.controlId)
      .onTouch((event: TouchEvent): void => {
        if (!this.gate.shouldHandleTouch(event.source, event.sourceTool)) return;
        event.stopPropagation();
        if (event.type === TouchType.Up) this.invoke();
      })
      .onMouse((event: MouseEvent): void => {
        if (!this.gate.shouldHandleMouse(event.source, event.sourceTool)) return;
        event.stopPropagation();
        if (event.action === MouseAction.Release) this.invoke();
      })
      .onClick((): void => this.invoke())
  }
}
