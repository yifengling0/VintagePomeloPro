import { skinColor } from '../service/SkinColors';

@Component
export struct SettingsSectionTitle {
  @Prop title: string = '';

  build() {
    Text(this.title).width('100%').fontSize(19).fontWeight(FontWeight.Bold)
      .fontColor(skinColor('text_primary'))
  }
}
