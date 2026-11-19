import { InputEventGate } from '../service/InputEventGate';
import { skinColor } from '../service/SkinColors';

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
      .fontColor(this.selected ? Color.White : this.darkPanel ? '#E8EDF5' : skinColor('text_primary'))
      .backgroundColor(this.selected ? '#2874BC' : this.darkPanel ? '#293140' : skinColor('search_background'))
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
