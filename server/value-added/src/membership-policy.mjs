import { requireThat } from './errors.mjs';
export const MEMBERSHIP_POLICY = 'calendar-month-v1';
export function addCalendarMonth(timestamp) {
  const date = new Date(timestamp), day = date.getUTCDate();
  date.setUTCDate(1); date.setUTCMonth(date.getUTCMonth() + 1);
  const end = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth() + 1, 0)).getUTCDate();
  date.setUTCDate(Math.min(day, end));
  return date.getTime();
}
/** Sort immutable verified order facts; recompute after a refund instead of subtracting a mutable counter. */
export function deriveMembership(orders, now, freshnessMs) {
  const purchased = Object.values(orders).filter(order => order.status === 'purchased')
    .sort((a, b) => a.purchasedAt - b.purchasedAt || a.orderId.localeCompare(b.orderId));
  let expiresAt = 0, fresh = true;
  for (const order of purchased) {
    requireThat(order.policyVersion === MEMBERSHIP_POLICY, 'MEMBERSHIP_POLICY_UNKNOWN', 503);
    if (order.purchasedAt >= expiresAt) fresh = true;
    fresh = fresh && order.verifiedAt + freshnessMs > now;
    expiresAt = addCalendarMonth(Math.max(order.purchasedAt, expiresAt));
  }
  return { active: expiresAt > now && fresh, expiresAt, serverTime: now, refreshRequired: expiresAt > now && !fresh };
}
