import { router } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { MembershipSupport } from '../components/MembershipSupport';
import { membershipLegalText, membershipLegalTitle } from '../model/MembershipLegal';
import { ValueAddedConfig } from '../model/ValueAddedModels';
import { skinColor } from '../service/SkinColors';

interface LegalParams { document?: string; }
@Entry
@Component
struct MembershipLegal {
  @State private title: string = '会员服务协议';
  @State private body: string = '';
  aboutToAppear(): void {
    const document = (router.getParams() as LegalParams)?.document ?? 'membership';
    this.title = membershipLegalTitle(document); this.body = membershipLegalText(document, '');
    const context = getContext(this) as common.UIAbilityContext;
    void context.resourceManager.getRawFileContent('value_added_config.json').then((bytes: Uint8Array): void => {
      const config = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as ValueAddedConfig;
      this.body = membershipLegalText(document, config.membershipOperatorName ?? '');
    }).catch((): void => {});
  }
  build() {
    Column({ space: 16 }) {
      Row({ space: 16 }) {
        Button('返回').onClick((): void => router.back())
        Text(this.title).fontSize(22).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
      }.width('100%').padding({ top: 44 })
      Scroll() {
        Column({ space: 24 }) {
          Text(this.body).fontSize(16).lineHeight(28).fontColor(skinColor('text_primary')).width('100%')
            .copyOption(CopyOptions.LocalDevice).id('membership-legal-body')
          MembershipSupport({ showHelpLink: this.title !== '客服与帮助' })
        }.width('100%').padding({ bottom: 40 }).alignItems(HorizontalAlign.Start)
      }.layoutWeight(1).width('100%')
    }.padding({ left: 24, right: 24 }).width('100%').height('100%').backgroundColor(skinColor('page_background'))
  }
}
