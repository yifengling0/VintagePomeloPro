import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { CoffeePurchaseService } from '../service/valueadded/CoffeePurchaseService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { skinColor } from '../service/SkinColors';
import { MembershipSupport } from './MembershipSupport';

@Component
export struct SponsorshipSettings {
  @Prop compact: boolean = false;
  @Prop surfaceColor: string | Resource = '';
  @Prop cardBorderColor: string = '#14000000';
  @Prop glass: boolean = false;
  @Prop groupPadding: number = 22;
  @State private expanded: boolean = false;
  @State private busy: boolean = false;
  @State private coffeeUnit: number = 8;
  @State private coffeeCurrency: string = 'CNY';
  @State private coffeeStatus: string = '';
  @State private status: string = '';
  private alive: boolean = false;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  aboutToAppear(): void { this.alive = true; }
  aboutToDisappear(): void { this.alive = false; }
  private async run(action: () => Promise<void>): Promise<void> {
    if (this.busy || !this.alive) return;
    this.busy = true; this.status = '';
    try { await action(); }
    finally { if (this.alive) this.busy = false; }
  }
  private toggleExpanded(): void {
    this.expanded = !this.expanded;
    if (this.expanded) { void this.loadCoffee(); this.recoverCoffee(true); }
  }
  private async loadCoffee(): Promise<void> {
    try {
      const service = CoffeePurchaseService.getInstance(); await service.initialize(this.context);
      const fallback = service.defaultPrice();
      if (this.alive) { this.coffeeUnit = fallback.amount; this.coffeeCurrency = fallback.currency; }
      const price = await service.price();
      if (this.alive) { this.coffeeUnit = price.amount; this.coffeeCurrency = price.currency; this.coffeeStatus = ''; }
    } catch (error) {
      if (this.alive) this.coffeeStatus = this.coffeeError(error as Error);
    }
  }
  private coffeePrice(quantity: number): string {
    const amount = this.coffeeUnit * quantity;
    return (this.coffeeCurrency === 'CNY' ? '¥' : this.coffeeCurrency + ' ') +
      (Number.isInteger(amount) ? '' + amount : amount.toFixed(2));
  }
  private coffeeError(error: Error): string {
    if (error.message === 'COFFEE_PRODUCT_MISSING') return '咖啡购买暂不可用，请稍后重试';
    if (error.message === 'COFFEE_PRICE_CHANGED') return '商品价格有变化，请刷新价格后重试';
    if (error.message === 'COFFEE_QUANTITY_UNSUPPORTED') return '当前系统暂不支持中杯和大杯，可选择小杯';
    if (error.message.startsWith('COFFEE_CONFIG')) return '咖啡商品配置需要更新';
    const sdkCode = (error as BusinessError).code;
    const code = Number.isInteger(sdkCode) ? sdkCode :
      /^COFFEE_IAP_[0-9]+$/.test(error.message) ? Number(error.message.slice('COFFEE_IAP_'.length)) : 0;
    if (code === 1001860003) return '咖啡购买暂不可用，请稍后重试';
    if (code === 1001860058 || error.message === 'COFFEE_IAP_1001860058') return '当前版本暂不可购买';
    return '购买暂未完成' + (code ? '（' + code + '）' : '') + '，请稍后重试';
  }
  private supportDeveloper(quantity: number): void {
    void this.run( async (): Promise<void> => {
      const service = CoffeePurchaseService.getInstance();
      try {
        await service.initialize(this.context);
        const result = await service.purchase(quantity, { amount: this.coffeeUnit, currency: this.coffeeCurrency });
        this.status = result === 'CANCELLED' ? '已取消支付' : result === 'ACK_PENDING' ?
          '订单确认尚未完成，请使用“检查未完成订单”，无需再次付款' :
          result === 'RECOVERED' ? '已确认之前的支持订单，感谢你！本次未发起新付款' : '感谢支持，咖啡订单已完成！';
      } catch (error) { this.status = this.coffeeError(error as Error); }
    });
  }
  private recoverCoffee(silent: boolean = false): void {
    void this.run( async (): Promise<void> => {
      try {
        const service = CoffeePurchaseService.getInstance(); await service.initialize(this.context);
        const result = await service.recoverPurchases();
        this.status = result === 'ACK_PENDING' ? '部分订单还未确认，请稍后再试，无需再次付款' :
          result === 'RECOVERED' ? '已确认之前的支持订单，感谢你！' : silent ? '' : '没有待确认的咖啡订单';
      } catch (error) { this.status = this.coffeeError(error as Error); }
    });
  }
  @Builder
  private CoffeeCup(title: string, quantity: number) {
    Button() {
      Column({ space: 10 }) {
        Stack() {
          Image($r('app.media.coffee_cup')).width(quantity === 1 ? 28 : quantity === 4 ? 32 : 36)
            .height(quantity === 1 ? 28 : quantity === 4 ? 32 : 36).objectFit(ImageFit.Contain)
            .fillColor(skinColor('accent'))
        }.width(40).height(40)
        Text(title).fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
        Text(this.coffeePrice(quantity)).fontSize(20).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
      }.width('100%').padding({ top: this.compact ? 16 : 20, bottom: this.compact ? 16 : 20 })
    }.type(ButtonType.Normal).layoutWeight(1).borderRadius(14)
      .backgroundColor(skinColor('search_background')).enabled(!this.busy)
      .accessibilityText(title + '咖啡，' + this.coffeePrice(quantity))
      .id('sync-coffee-' + quantity).onClick((): void => this.supportDeveloper(quantity))
  }
  build() {
    if (DeviceCapabilityPolicy.supportsValueAddedFeatures()) {
      Column({ space: 16 }) {
        Button() {
          Row({ space: 14 }) {
            Stack() {
              Image($r('app.media.coffee_cup')).width(26).height(26).objectFit(ImageFit.Contain)
                .fillColor(skinColor('accent'))
            }.width(44).height(44).borderRadius(14).backgroundColor(skinColor('search_background'))
            Column({ space: 6 }) {
              Text('赞助').fontSize(18).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
              Text('喜欢旧柚的话，可以请开发者喝杯咖啡').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            SymbolGlyph(this.expanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
              .fontSize(16).fontColor([skinColor('text_secondary')])
          }.width('100%').constraintSize({ minHeight: 54 })
        }.type(ButtonType.Normal).width('100%').padding(0).borderRadius(12)
          .backgroundColor(Color.Transparent).id('settings-support-expand')
          .accessibilityText(this.expanded ? '收起赞助选项' : '展开赞助选项')
          .onClick((): void => this.toggleExpanded())
        if (this.expanded) {
          Divider().color(this.cardBorderColor)
          Row({ space: 12 }) {
            this.CoffeeCup('小杯', 1)
            this.CoffeeCup('中杯', 4)
            this.CoffeeCup('大杯', 10)
          }.width('100%').id('settings-support-options')
          Text('自愿支持开发，不包含会员权益。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
          MembershipSupport()
          if (this.coffeeStatus) { Text(this.coffeeStatus).fontSize(12).fontColor(skinColor('text_secondary')).width('100%') }
          if (this.busy) { LoadingProgress().width(18).height(18).color(skinColor('accent')) }
          if (this.status) { Text(this.status).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('settings-support-status') }
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
            Button('刷新价格').height(40).fontSize(12).borderRadius(12).enabled(!this.busy)
              .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_secondary'))
              .id('sync-coffee-refresh').onClick((): void => { void this.loadCoffee(); })
            Button('检查未完成订单').height(40).fontSize(12).borderRadius(12).enabled(!this.busy).margin({ left: 8 })
              .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_secondary'))
              .id('sync-coffee-recover').onClick((): void => this.recoverCoffee())
          }.width('100%')
        }
      }.width('100%').padding(this.groupPadding).borderRadius(20).alignItems(HorizontalAlign.Start)
        .backgroundColor(this.surfaceColor || skinColor('card_background'))
        .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.cardBorderColor }).id('settings-group-support')
    }
  }
}
