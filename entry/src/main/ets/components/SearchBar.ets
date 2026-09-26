@Component
export struct AppSearchBar {
  @Prop value: string = '';
  @Prop heightVp: number = 48;
  placeholder: string = '搜索 Windows 应用';
  onValueChange: (value: string) => void = (_value: string): void => {};

  build() {
    Search({ value: this.value, placeholder: this.placeholder })
      // 高度自适应 + minHeight 保底: 大字体设备上固定 vp 高会裁字
      .constraintSize({ minHeight: this.heightVp })
      .width('100%')
      .backgroundColor($r('app.color.search_background'))
      .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
      .borderRadius(24)
      .border({ width: 1, color: '#18FFFFFF' })
      .padding({ left: 6, right: 6 })
      .onChange((value: string) => this.onValueChange(value))
  }
}
