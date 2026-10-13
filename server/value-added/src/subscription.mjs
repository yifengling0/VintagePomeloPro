import { createCipheriv, createDecipheriv, createHash, randomBytes } from 'node:crypto';
import { requireThat } from './errors.mjs';
import { SUBSCRIPTION_POLICY } from './membership-policy.mjs';

const identifier = value => typeof value === 'string' && value.length > 0 && value.length <= 256;
const timestamp = value => Number.isSafeInteger(value) && value > 0;
function identity(order, config, candidate) {
  requireThat(order && order.applicationId === config.appId && order.environment === config.environment &&
    order.productType === '2' && order.productId === candidate.productId &&
    order.purchaseToken === candidate.purchaseToken && order.subscriptionId === candidate.subscriptionId &&
    order.subGroupId === candidate.subGroupId && order.subGroupGenerationId === candidate.subGroupGenerationId &&
    identifier(order.purchaseOrderId), 'INVALID_SUBSCRIPTION');
}

/** Input is the verified official group JWS. Client receipt dates never grant subscription time. */
export function subscriptionFact(group, candidate, config, now) {
  requireThat(group && group.applicationId === config.appId && group.packageName === config.packageName &&
    group.environment === config.environment && group.subGroupId === candidate.subGroupId &&
    identifier(candidate.subscriptionId) && identifier(candidate.subGroupGenerationId) && identifier(candidate.subGroupId),
    'INVALID_SUBSCRIPTION');
  const product = config.products.find(value => value.id === candidate.productId && value.type === 2);
  requireThat(product && product.kind === 'membership' && product.policyVersion === SUBSCRIPTION_POLICY &&
    product.subscriptionGroupId === group.subGroupId, 'UNKNOWN_PRODUCT');
  const last = group.lastSubscriptionStatus;
  const history = group.historySubscriptionStatusList ?? [];
  requireThat(Array.isArray(history) && history.length <= 10, 'INVALID_SUBSCRIPTION');
  const current = [last, ...history].find(value => value && value.subscriptionId === candidate.subscriptionId &&
    value.purchaseToken === candidate.purchaseToken && value.subGroupGenerationId === candidate.subGroupGenerationId);
  requireThat(current && ['1', '2', '3', '5'].includes(current.status) && timestamp(current.expiresTime), 'INVALID_SUBSCRIPTION');
  const order = current.lastPurchaseOrder;
  identity(order, config, candidate);
  requireThat(timestamp(order.purchaseTime) && order.purchaseTime <= now + 60000 && timestamp(order.signedTime) &&
    order.signedTime <= now + 60000 && current.expiresTime > order.purchaseTime && order.duration === 'P1M' &&
    (order.quantity === undefined || order.quantity === 1), 'INVALID_SUBSCRIPTION');
  requireThat(typeof order.currency === 'string' && Number.isSafeInteger(order.price) &&
    (product.prices?.[order.currency]?.includes(order.price) || order.offerTypeCode === '2' &&
      typeof order.offerId === 'string' && Object.hasOwn(product.promotionalPrices ?? {}, order.offerId) &&
      product.promotionalPrices[order.offerId]?.[order.currency]?.includes(order.price)), 'ORDER_PRICE_MISMATCH');
  const renewal = current.renewalInfo;
  requireThat(renewal && renewal.environment === config.environment && renewal.productId === order.productId &&
    renewal.subGroupGenerationId === current.subGroupGenerationId && ['0', '1'].includes(renewal.autoRenewStatusCode) &&
    typeof renewal.hasInBillingRetryPeriod === 'boolean', 'INVALID_SUBSCRIPTION');
  const revoked = current.status === '5' || order.revocationTime !== undefined || order.purchaseOrderRevocationReasonCode !== undefined;
  requireThat(revoked || ['1', '2'].includes(order.finishStatus), 'ORDER_FINISH_UNKNOWN');
  const isLatest = last?.subscriptionId === current.subscriptionId && last?.purchaseToken === current.purchaseToken;
  // Billing retry is not proof of paid access; no undocumented grace period is invented.
  const status = revoked ? 'revoked' : current.status === '1' && isLatest && current.expiresTime > now ? 'purchased' : 'expired';
  // Renewals may carry no developerPayload. The signed original receipt supplies the immutable owner binding.
  requireThat(!order.developerPayload || !candidate.developerPayload || order.developerPayload === candidate.developerPayload,
    'ORDER_BINDING_MISMATCH', 403);
  return { appId: config.appId, orderId: order.purchaseOrderId, productId: order.productId, productType: 2,
    purchaseToken: current.purchaseToken, developerPayload: candidate.developerPayload || order.developerPayload, quantity: 1, status,
    needsFinish: !revoked && order.finishStatus === '2', purchasedAt: order.purchaseTime, expiresAt: current.expiresTime,
    subscriptionId: current.subscriptionId, subscriptionGroupId: group.subGroupId,
    generationId: current.subGroupGenerationId, subscriptionStatus: current.status,
    autoRenew: renewal.autoRenewStatusCode === '1', billingRetry: renewal.hasInBillingRetryPeriod,
    signedAt: order.signedTime, policyVersion: SUBSCRIPTION_POLICY };
}

// CloudDB stores only authenticated ciphertext for automatic server rechecks. Secrets never enter responses or logs.
const key = secret => createHash('sha256').update('vp-subscription-receipt-v1\n' + secret).digest();
export function sealReceipt(receipt, secret, owner) {
  const iv = randomBytes(12), cipher = createCipheriv('aes-256-gcm', key(secret), iv);
  cipher.setAAD(Buffer.from(owner));
  const encrypted = Buffer.concat([cipher.update(receipt, 'utf8'), cipher.final()]);
  return [iv, cipher.getAuthTag(), encrypted].map(value => value.toString('base64url')).join('.');
}
export function openReceipt(value, secret, owner) {
  requireThat(typeof value === 'string' && value.length < 180000, 'INVALID_SUBSCRIPTION');
  const parts = value.split('.');
  requireThat(parts.length === 3 && parts.every(part => /^[A-Za-z0-9_-]+$/.test(part)), 'INVALID_SUBSCRIPTION');
  const [iv, tag, encrypted] = parts.map(part => Buffer.from(part, 'base64url'));
  requireThat(iv.length === 12 && tag.length === 16, 'INVALID_SUBSCRIPTION');
  const cipher = createDecipheriv('aes-256-gcm', key(secret), iv);
  cipher.setAAD(Buffer.from(owner)); cipher.setAuthTag(tag);
  return Buffer.concat([cipher.update(encrypted), cipher.final()]).toString('utf8');
}
