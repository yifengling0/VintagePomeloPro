import { pasteboard } from '@kit.BasicServicesKit';
import { router } from '@kit.ArkUI';
import { MEMBERSHIP_SUPPORT_EMAIL, MEMBERSHIP_SUPPORT_GROUP } from '../model/MembershipCommerce';
import { skinColor } from '../service/SkinColors';

@Component
export struct MembershipSupport {
  @Prop showHelpLink: boolean = true;
  @State private message: string = '';
  private async copy(value: string): Promise<void> {
    try {
      await pasteboard.getSystemPasteboard().setData(pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, value));
      this.message = '已复制';
    } catch (_) { this.message = '复制未完成，可长按联系方式复制'; }
  }
  build() {
    Column({ space: 8 }) {
      Text('客服邮箱 ' + MEMBERSHIP_SUPPORT_EMAIL).fontSize(13).fontColor(skinColor('text_primary')).copyOption(CopyOptions.LocalDevice)
      Text('QQ 群 ' + MEMBERSHIP_SUPPORT_GROUP).fontSize(13).fontColor(skinColor('text_primary')).copyOption(CopyOptions.LocalDevice)
      Flex({ wrap: FlexWrap.Wrap }) {
        Button('复制邮箱').fontSize(12).onClick((): void => { void this.copy(MEMBERSHIP_SUPPORT_EMAIL); }).id('membership-copy-email')
        Button('复制群号').fontSize(12).margin({ left: 8 }).onClick((): void => { void this.copy(MEMBERSHIP_SUPPORT_GROUP); }).id('membership-copy-group')
        if (this.showHelpLink) {
          Button('客服与帮助').fontSize(12).margin({ left: 8 }).onClick((): void => {
            void router.pushUrl({ url: 'pages/MembershipLegal', params: { document: 'support' } });
          }).id('membership-support')
        }
      }
      if (this.message) { Text(this.message).fontSize(12).fontColor(skinColor('text_secondary')) }
    }.width('100%').alignItems(HorizontalAlign.Start)
  }
}
