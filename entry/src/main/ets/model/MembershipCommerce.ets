/** A confirmation belongs to one account, quote and document revision; never persist it. */
export const MEMBERSHIP_TERMS_VERSION: string = '2026-10-16';
export const MEMBERSHIP_SUPPORT_EMAIL: string = 'liufengpm@live.cn';
export const MEMBERSHIP_SUPPORT_GROUP: string = '966307250';
export interface MembershipQuote {
  id: string;
  type: number;
  price: string;
  microPrice?: number;
  currency?: string;
  regularPrice?: string;
  priceDescription?: string;
}
export interface MembershipConfirmation {
  userId: string;
  quote: string;
  termsVersion: string;
  membershipAccepted: boolean;
  autoRenewAccepted: boolean;
}
export function membershipQuoteKey(product: MembershipQuote, promotion: string = ''): string {
  return JSON.stringify([product.id, product.type, product.price, product.microPrice ?? 0,
    product.currency ?? '', product.regularPrice ?? '', product.priceDescription ?? '', promotion]);
}
export function validateMembershipConfirmation(value: MembershipConfirmation | undefined,
  userId: string, quote: string, automatic: boolean): void {
  if (!value || !userId || value.userId !== userId || value.quote !== quote ||
    value.termsVersion !== MEMBERSHIP_TERMS_VERSION || value.membershipAccepted !== true ||
    automatic && value.autoRenewAccepted !== true) throw new Error('MEMBERSHIP_CONSENT_REQUIRED');
}
