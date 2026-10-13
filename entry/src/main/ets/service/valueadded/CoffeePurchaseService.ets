import { common } from '@kit.AbilityKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { iap } from '@kit.IAPKit';
import util from '@ohos.util';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';

interface CoffeeConfig { productId: string; appId: string; unitPriceCny: number; }
interface CoffeeReceipt { jwsPurchaseOrder: string; }
interface CoffeeOrder { productId: string; applicationId: string; productType: string; quantity?: number; purchaseToken: string; purchaseOrderId: string; }
interface CoffeeRecovery { recovered: number; pending: number; }
export interface CoffeePrice { amount: number; currency: string; }

/** VintagePomelo's consumable coffee flow (1/4/10 units). It never grants cloud membership. */
export class CoffeePurchaseService {
  private static instance: CoffeePurchaseService | null = null;
  private context: common.UIAbilityContext | null = null;
  private config: CoffeeConfig | null = null;
  private busy: boolean = false;
  static getInstance(): CoffeePurchaseService {
    if (!CoffeePurchaseService.instance) CoffeePurchaseService.instance = new CoffeePurchaseService();
    return CoffeePurchaseService.instance;
  }
  private guard(): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN');
  }
  async initialize(context: common.UIAbilityContext): Promise<void> {
    this.guard();
    if (this.config) return;
    const bytes = await context.resourceManager.getRawFileContent('coffee_config.json');
    const config = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as CoffeeConfig;
    if (!/^[a-zA-Z0-9._-]{1,128}$/.test(config.productId) || typeof config.appId !== 'string' ||
      config.appId.length > 128 || !Number.isFinite(config.unitPriceCny) || config.unitPriceCny <= 0) throw new Error('COFFEE_CONFIG_INVALID');
    this.context = context; this.config = config;
  }
  async price(): Promise<CoffeePrice> {
    this.guard();
    if (!this.context || !this.config) throw new Error('COFFEE_CONFIG_INVALID');
    const products = await iap.queryProducts(this.context, { productType: iap.ProductType.CONSUMABLE,
      productIds: [this.config.productId] });
    const product = products.find((item: iap.Product): boolean => item.id === this.config?.productId && item.type === iap.ProductType.CONSUMABLE);
    if (!product) throw new Error('COFFEE_PRODUCT_MISSING');
    if (!Number.isSafeInteger(product.microPrice) || product.microPrice <= 0 || !/^[A-Z]{3}$/.test(product.currency)) throw new Error('COFFEE_PRICE_INVALID');
    return { amount: product.microPrice / 1000000, currency: product.currency };
  }
  defaultPrice(): CoffeePrice { this.guard(); return { amount: this.config?.unitPriceCny ?? 8, currency: 'CNY' }; }
  private order(data: string): CoffeeOrder {
    if (typeof data !== 'string' || data.length > 1048576) throw new Error('COFFEE_ORDER_INVALID');
    const receipt = JSON.parse(data) as CoffeeReceipt;
    if (typeof receipt.jwsPurchaseOrder !== 'string') throw new Error('COFFEE_ORDER_INVALID');
    const parts = receipt.jwsPurchaseOrder.split('.');
    if (parts.length !== 3 || !/^[A-Za-z0-9_-]+$/.test(parts[1])) throw new Error('COFFEE_ORDER_INVALID');
    let base64 = parts[1].replace(/-/g, '+').replace(/_/g, '/');
    while (base64.length % 4) base64 += '=';
    const bytes = new util.Base64Helper().decodeSync(base64);
    const order = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as CoffeeOrder;
    if (typeof order.productId !== 'string' || typeof order.purchaseToken !== 'string' || !order.purchaseToken ||
      typeof order.purchaseOrderId !== 'string' || !order.purchaseOrderId) throw new Error('COFFEE_ORDER_INVALID');
    return order;
  }
  private async finish(order: CoffeeOrder, quantity: number = 0): Promise<void> {
    if (!this.context || !this.config || order.productId !== this.config.productId ||
      this.config.appId && order.applicationId !== this.config.appId || order.productType !== '0' ||
      !Number.isInteger(order.quantity ?? 1) || (order.quantity ?? 1) < 1 || (order.quantity ?? 1) > 10 ||
      quantity > 0 && (order.quantity ?? 1) !== quantity) throw new Error('COFFEE_ORDER_INVALID');
    // Decoding only retrieves SDK receipt references. Success requires Huawei to acknowledge the actual order.
    // No externally supplied receipt is accepted and no membership is derived from the decoded payload.
    try {
      await iap.finishPurchase(this.context, { productType: iap.ProductType.CONSUMABLE,
        purchaseToken: order.purchaseToken, purchaseOrderId: order.purchaseOrderId });
    } catch (error) {
      const code = (error as BusinessError).code;
      // Old VintagePomelo treats finalized-order errors as idempotent. Confirm absence from
      // official UNFINISHED first: a generic order-state error alone is not delivery success.
      if ((code === 1001860030 || code === 1001860050) && !await this.isUnfinished(order.purchaseOrderId)) return;
      const diagnostic = Number.isInteger(code) ? '' + code :
        /^[A-Z_]{2,64}$/.test((error as Error).message) ? (error as Error).message : 'FAILED';
      console.warn('[CoffeePurchaseService] order confirmation failed: ' + diagnostic);
      throw error as Error;
    }
  }
  private async isUnfinished(orderId: string): Promise<boolean> {
    if (!this.context) throw new Error('COFFEE_CONFIG_INVALID');
    let cursor = '';
    const cursors = new Set<string>();
    do {
      const page = await iap.queryPurchases(this.context, { productType: iap.ProductType.CONSUMABLE,
        queryType: iap.PurchaseQueryType.UNFINISHED, continuationToken: cursor || undefined });
      for (const data of page.purchaseDataList) if (this.order(data).purchaseOrderId === orderId) return true;
      cursor = page.continuationToken ?? '';
      if (cursor && (cursors.has(cursor) || cursors.size >= 32)) throw new Error('COFFEE_ORDER_INVALID');
      cursors.add(cursor);
    } while (cursor);
    return false;
  }
  private async recover(): Promise<CoffeeRecovery> {
    if (!this.context || !this.config) throw new Error('COFFEE_CONFIG_INVALID');
    const result: CoffeeRecovery = { recovered: 0, pending: 0 };
    let cursor = '';
    const cursors = new Set<string>(), orders = new Set<string>();
    do {
      const page = await iap.queryPurchases(this.context, { productType: iap.ProductType.CONSUMABLE,
        queryType: iap.PurchaseQueryType.UNFINISHED, continuationToken: cursor || undefined });
      for (const data of page.purchaseDataList) {
        const order = this.order(data);
        if (order.productId !== this.config.productId || orders.has(order.purchaseOrderId)) continue;
        orders.add(order.purchaseOrderId);
        try { await this.finish(order); result.recovered++; } catch (_) { result.pending++; }
      }
      cursor = page.continuationToken ?? '';
      if (cursor && (cursors.has(cursor) || cursors.size >= 32)) throw new Error('COFFEE_ORDER_INVALID');
      cursors.add(cursor);
    } while (cursor);
    return result;
  }
  async recoverPurchases(): Promise<string> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const result = await this.recover();
      return result.pending ? 'ACK_PENDING' : result.recovered ? 'RECOVERED' : 'NO_PENDING';
    } finally { this.busy = false; }
  }
  async purchase(quantity: number, expected: CoffeePrice): Promise<string> {
    this.guard();
    if (!this.context || !this.config) throw new Error('COFFEE_CONFIG_INVALID');
    if (![1, 4, 10].includes(quantity)) throw new Error('COFFEE_QUANTITY_INVALID');
    if (quantity > 1 && deviceInfo.sdkApiVersion < 15) throw new Error('COFFEE_QUANTITY_UNSUPPORTED');
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      // Check paid-but-unfinished orders before allowing another charge.
      const pending = await this.recover();
      if (pending.pending) return 'ACK_PENDING';
      if (pending.recovered) return 'RECOVERED';
      const price = await this.price();
      if (price.amount !== expected.amount || price.currency !== expected.currency) throw new Error('COFFEE_PRICE_CHANGED');
      let result: iap.CreatePurchaseResult;
      try {
        result = await iap.createPurchase(this.context, { productType: iap.ProductType.CONSUMABLE,
          productId: this.config.productId, quantity: quantity });
      } catch (error) {
        const code = (error as BusinessError).code;
        if (code === iap.IAPErrorCode.USER_CANCELED) return 'CANCELLED';
        if (code === iap.IAPErrorCode.PRODUCT_OWNED || code === iap.IAPErrorCode.SYSTEM_ERROR || code === 1001860001) {
          const recovered = await this.recover();
          if (recovered.recovered && !recovered.pending) return 'RECOVERED';
          if (recovered.pending || code === iap.IAPErrorCode.PRODUCT_OWNED) return 'ACK_PENDING';
        }
        throw new Error('COFFEE_IAP_' + (Number.isInteger(code) ? code : 'FAILED'));
      }
      const order = this.order(result.purchaseData);
      try { await this.finish(order, quantity); return 'FINISHED'; }
      catch (_) { return 'ACK_PENDING'; }
    } finally { this.busy = false; }
  }
}
