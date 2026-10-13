import { requireThat } from './errors.mjs';
export const MEMBERSHIP_POLICY = 'calendar-month-v1';
export const SUBSCRIPTION_POLICY = 'huawei-subscription-v1';
export function addCalendarMonth(timestamp) {
  const date = new Date(timestamp), day = date.getUTCDate();
  date.setUTCDate(1); date.setUTCMonth(date.getUTCMonth() + 1);
  const end = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth() + 1, 0)).getUTCDate();
  date.setUTCDate(Math.min(day, end));
  return date.getTime();
}
/** Sort immutable verified order facts; recompute after a refund instead of subtracting a mutable counter. */
export function deriveMembership(orders, now, freshnessMs, subscriptions = {}) {
  const purchased = Object.values(orders).filter(order => order.status === 'purchased')
    .sort((a, b) => a.purchasedAt - b.purchasedAt || a.orderId.localeCompare(b.orderId));
  let expiresAt = 0, fresh = true;
  for (const order of purchased) {
    requireThat(order.policyVersion === MEMBERSHIP_POLICY, 'MEMBERSHIP_POLICY_UNKNOWN', 503);
    if (order.purchasedAt >= expiresAt) fresh = true;
    fresh = fresh && order.verifiedAt + freshnessMs > now;
    expiresAt = addCalendarMonth(Math.max(order.purchasedAt, expiresAt));
  }
  const passes = { active: expiresAt > now && fresh, expiresAt, refreshRequired: expiresAt > now && !fresh };
  const states = Object.values(subscriptions);
  for (const state of states) requireThat(state.policyVersion === SUBSCRIPTION_POLICY, 'MEMBERSHIP_POLICY_UNKNOWN', 503);
  // A subscription is one authoritative snapshot, never an extra month per receipt or renewal.
  const current = states.filter(state => state.status === 'purchased' && state.expiresAt > now)
    .sort((a, b) => b.expiresAt - a.expiresAt);
  const verified = current.find(state => state.verifiedAt + freshnessMs > now);
  const latest = verified ?? current[0] ?? states.sort((a, b) => b.verifiedAt - a.verifiedAt)[0];
  return { active: passes.active || !!verified, expiresAt: Math.max(passes.expiresAt, latest?.expiresAt ?? 0),
    serverTime: now, refreshRequired: !passes.active && !verified && (passes.refreshRequired || current.length > 0),
    ...(latest ? { subscriptionStatus: latest.subscriptionStatus, autoRenew: latest.autoRenew,
      billingRetry: latest.billingRetry } : {}) };
}
