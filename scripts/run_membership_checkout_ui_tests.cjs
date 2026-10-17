#!/usr/bin/env node
// Run the component's real checkout methods without ArkUI rendering or a payment SDK.
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(process.argv[2]);
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/components/AccountSyncSettings.ets'), 'utf8');
const names = ['buy', 'errorMessage', 'showPurchaseMessage', 'canBuyMembership', 'clearConfirmation', 'refreshReminder', 'openCheckout', 'selectProduct', 'checkoutProduct',
  'manageMembership', 'restoreMembership', 'applyMembershipState', 'expiryDate'];
const methods = names.map(name => {
  const match = new RegExp('^  private (?:async )?' + name + '\\(', 'm').exec(source);
  assert.ok(match, name);
  const rest = source.slice(match.index);
  const end = /\n  (?:private |@Builder)/.exec(rest.slice(1));
  assert.ok(end, name);
  return rest.slice(0, end.index + 1);
}).join('\n');
const product = { id: 'monthly', type: 2, price: '¥5.00' };
let owner, capabilities, ready, permissionCalls, cashierCalls, refreshCalls, enabled, permissionAction;
let purchaseError, purchaseResult, dialogCalls, toastCalls, dialogAction, accountError;
let environmentError, environmentCalls;
let subscription, confirmError, restoreError, manageAction, confirmAction, restoreAction, confirmCalls, restoreCalls;
const prompt = {
  showDialog: async options => { dialogCalls.push(options); if (dialogAction) await dialogAction(); },
  showToast: options => toastCalls.push(options)
};
const service = {
  getUserId: () => owner, getCapabilities: () => capabilities, getMembershipOperator: () => 'Publisher',
  getPurchaseRecoveryStatus: () => '',
  manageSubscriptions: async () => { if (manageAction) await manageAction(); },
  restorePurchases: async () => { restoreCalls++; if (restoreAction) await restoreAction(); if (restoreError) throw restoreError; },
  confirmSubscriptionState: async () => {
    confirmCalls++; if (confirmAction) await confirmAction(); if (confirmError) throw confirmError; return subscription;
  },
  quoteKey: p => p.id + ':' + p.price,
  checkPurchaseEnvironment: async () => { environmentCalls++; if (environmentError) throw environmentError; },
  refreshCapabilities: async () => { refreshCalls++; },
  notificationsEnabled: async () => enabled,
  prepareRenewalReminders: async request => { permissionCalls.push(request); if (permissionAction) await permissionAction(); return ready; },
  purchase: async (p, confirmation) => {
    assert.equal(confirmation.userId, owner); assert.equal(confirmation.quote, service.quoteKey(p));
    assert.equal(confirmation.membershipAccepted, true);
    if (p.type === 2) assert.equal(confirmation.autoRenewAccepted, true);
    cashierCalls++;
    if (purchaseError) throw purchaseError;
    return purchaseResult;
  }
};
const js = ts.transpileModule('class Harness {\n' + methods + '\n}\nmodule.exports = Harness;',
  { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
const mod = { exports: {} };
vm.runInNewContext(js, { module: mod, ValueAddedService: { getInstance: () => service },
  skinColor: () => '#5569D8',
  console: { warn() {} },
  MEMBERSHIP_TERMS_VERSION: '2026-10-16', AccountAutomaticSyncService: { getInstance: () => ({ requestSync() {} }) } });
function harness() {
  owner = 'alice'; capabilities = { commerce: true, membershipPurchases: true, renewalReminders: true };
  ready = true; enabled = true; permissionCalls = []; cashierCalls = 0; refreshCalls = 0; permissionAction = null;
  purchaseError = null; purchaseResult = 'CANCELLED'; dialogCalls = []; toastCalls = []; dialogAction = null; accountError = null;
  environmentError = null; environmentCalls = 0;
  subscription = { active: true, autoRenew: false, expiresAt: Date.now() + 86400000, serverTime: Date.now() };
  confirmError = restoreError = manageAction = confirmAction = restoreAction = null; confirmCalls = restoreCalls = 0;
  const ui = Object.assign(new mod.exports(), { alive: true, busy: false, userId: owner, commerceReady: true,
    membershipAccepted: false, autoRenewAccepted: false, selectedProductId: product.id, reminderReady: false,
    showCheckout: true, products: [{ ...product, purpose: 'membership' }], vipActive: false, membershipUncertain: false, membershipRequest: 0 });
  ui.applyVip = (value, account) => { assert.equal(account, owner); ui.applied = value; ui.vipActive = value.active && !value.refreshRequired; };
  ui.getUIContext = () => ({ getPromptAction: () => prompt });
  ui.refreshAccount = async () => { if (accountError) throw accountError; };
  ui.refreshMembershipProducts = async () => ui.clearConfirmation();
  ui.run = (_, work) => { ui.busy = true; ui.pending = work().finally(() => { ui.busy = false; }); return ui.pending; };
  return ui;
}
(async () => {
  let ui = harness();
  ui.membershipAccepted = ui.autoRenewAccepted = true; ui.showCheckout = false;
  assert.equal(ui.canBuyMembership(product), false, 'the product page cannot pay without confirmation');
  ui.openCheckout(); assert.equal(ui.showCheckout, true);
  assert.equal(ui.membershipAccepted, false); assert.equal(ui.autoRenewAccepted, false, 'each checkout starts without consent');
  ui.showCheckout = false; ui.selectedProductId = '';
  ui.openCheckout(); assert.equal(ui.selectedProductId, product.id, 'the explicit open button selects the sole available plan');
  ui.buy(product); assert.equal(ui.pending, undefined); assert.equal(cashierCalls, 0);
  ui.membershipAccepted = true; assert.equal(ui.canBuyMembership(product), false);
  ui.autoRenewAccepted = true;
  assert.equal(ui.canBuyMembership(product), true, 'permissions not prepared yet must not deadlock the payment button');
  ui.buy(product); await ui.pending;
  assert.deepEqual(permissionCalls, [true]); assert.equal(refreshCalls, 1); assert.equal(cashierCalls, 1);
  assert.equal(environmentCalls, 1, 'official payment environment is checked before notification preflight');
  assert.equal(ui.membershipAccepted, false, 'leaving the cashier resets confirmation');
  assert.equal(dialogCalls.length, 0, 'normal cashier cancellation is not an error');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; ready = false; enabled = false;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0); assert.match(ui.status, /通知/); assert.equal(ui.membershipAccepted, true);
  assert.equal(dialogCalls.length, 1); assert.match(dialogCalls[0].message, /未发起支付，没有扣款/);
  assert.match(dialogCalls[0].message, /系统设置/);
  ready = true; enabled = true; ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 1, 'granting permission after denial permits retry');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; capabilities.renewalReminders = false;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0); assert.equal(permissionCalls.length, 0);
  assert.equal(dialogCalls.length, 1); assert.equal(dialogCalls[0].title, '暂时无法开通');
  assert.match(dialogCalls[0].message, /续费提醒服务暂不可用/); assert.match(ui.status, /未发起支付，没有扣款/);
  assert.equal(ui.busy, false, 'dismissal permits a later retry');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  capabilities.renewalReminders = false; capabilities.sandboxCommerce = true; capabilities.sandboxCheckout = true;
  assert.equal(await ui.refreshReminder(false), false, 'sandbox checkout is not reminder readiness');
  assert.match(ui.reminderStatus, /沙盒.*不会真实扣款/);
  ui.buy(product); await ui.pending;
  assert.equal(environmentCalls, 1); assert.equal(cashierCalls, 1); assert.equal(permissionCalls.length, 0);
  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  capabilities.renewalReminders = false; capabilities.sandboxCheckout = true;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0, 'test flag without the sandbox environment cannot skip real renewal preflight');
  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  capabilities.sandboxCommerce = true; capabilities.sandboxCheckout = true;
  environmentError = Object.assign(new Error('sandbox inactive'), {code:1001860058});
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0, 'official sandbox activation remains mandatory');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; ready = false;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0); assert.match(dialogCalls[0].message, /登记未完成/);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  purchaseError = Object.assign(new Error('store unavailable'), { code: 18600001 });
  ui.buy(product); await ui.pending;
  assert.equal(dialogCalls[0].title, '购买未完成'); assert.match(dialogCalls[0].message, /18600001/);
  assert.match(dialogCalls[0].message, /恢复购买/); assert.doesNotMatch(dialogCalls[0].message, /没有扣款/);
  assert.equal(ui.membershipAccepted, false); assert.equal(ui.autoRenewAccepted, false);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; purchaseError = new Error('PRICE_CHANGED');
  ui.buy(product); await ui.pending;
  assert.match(dialogCalls[0].message, /商品优惠已变化/); assert.equal(ui.showCheckout, false);

  for (const [code, reason] of [[1001860002, /包名及签名/], [1001860057, /调试签名/], [1001860058, /测试名单/]]) {
    ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
    environmentError = Object.assign(new Error('opaque SDK diagnostic'), { code });
    ui.buy(product); await ui.pending;
    assert.equal(cashierCalls, 0); assert.equal(permissionCalls.length, 0);
    assert.match(dialogCalls[0].message, reason); assert.match(dialogCalls[0].message, new RegExp(''+code));
    assert.match(dialogCalls[0].message, /未发起支付/);
    assert.doesNotMatch(dialogCalls[0].message, /opaque SDK diagnostic/);
  }

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; purchaseResult = 'VERIFICATION_PENDING';
  ui.buy(product); await ui.pending;
  assert.equal(dialogCalls.length, 1); assert.match(dialogCalls[0].title, /会员待确认/);
  assert.match(dialogCalls[0].message, /无需再次付款/); assert.doesNotMatch(dialogCalls[0].message, /没有扣款/);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  purchaseResult = 'FINISHED'; accountError = new Error('offline');
  ui.buy(product); await ui.pending;
  assert.equal(dialogCalls[0].title, '会员状态待确认'); assert.match(ui.status, /无需再次付款/);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; capabilities.renewalReminders = false;
  dialogAction = async () => { throw new Error('window unavailable'); };
  ui.buy(product); await ui.pending;
  assert.equal(toastCalls.length, 1, 'a failed dialog keeps a visible toast and page outcome');
  assert.match(ui.status, /未发起支付/);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; capabilities.renewalReminders = false;
  let dismiss;
  dialogAction = () => new Promise(resolve => { dismiss = resolve; });
  ui.buy(product);
  for (let i = 0; i < 10 && !dismiss; i++) await Promise.resolve();
  assert.equal(typeof dismiss, 'function'); assert.equal(ui.busy, true);
  const pending = ui.pending; ui.buy(product); assert.equal(ui.pending, pending, 'no duplicate attempt while an outcome is shown');
  dismiss(); await pending; assert.equal(dialogCalls.length, 1); assert.equal(cashierCalls, 0);

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  permissionAction = async () => { owner = 'bob'; ui.userId = owner; ui.clearConfirmation(); };
  ui.buy(product); await ui.pending; assert.equal(cashierCalls, 0, 'account change during permission cannot pay');
  assert.equal(dialogCalls.length, 0, 'do not show the old account outcome after switching accounts');

  ui = harness(); ui.membershipAccepted = true; ui.selectedProductId = 'once';
  ui.buy({ id: 'once', type: 3, price: '¥5.00' }); await ui.pending;
  assert.equal(cashierCalls, 1); assert.equal(permissionCalls.length, 0, 'single month requires no renewal permission');
  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  ui.selectProduct({ id: 'once', type: 3, price: '¥5.00' });
  assert.equal(ui.showCheckout, false); assert.equal(ui.membershipAccepted, false); assert.equal(ui.autoRenewAccepted, false);

  ui = harness(); restoreError = new Error('INVALID_SUBSCRIPTION');
  ui.manageMembership(); await ui.pending;
  assert.equal(restoreCalls, 0, 'management must query official state independently of historical receipt recovery');
  assert.equal(confirmCalls, 1); assert.match(ui.status, /自动续费已关闭，权益保留至/);
  assert.match(ui.membership, /到期后不再续费/); assert.doesNotMatch(ui.renewalDetail, /预计续费/);
  assert.equal(ui.vipActive, true, 'cancellation preserves the verified paid period');
  assert.equal(ui.membershipAccepted, false); assert.equal(ui.autoRenewAccepted, false);

  ui = harness(); subscription.active = false; subscription.subscriptionStatus = '2';
  ui.manageMembership(); await ui.pending;
  assert.equal(ui.status, '自动续费已关闭，会员已到期', 'a verified expired subscription is not an unknown result');
  assert.equal(ui.renewalDetail, '', 'an expired subscription must not promise paid access until a future sandbox date');

  ui = harness(); subscription.autoRenew = true;
  ui.manageMembership(); await ui.pending;
  assert.match(ui.status, /仍然开启/, 'closing the dialog is not cancellation');
  ui = harness(); confirmError = new Error('PROVIDER_UNAVAILABLE');
  ui.manageMembership(); await ui.pending;
  assert.match(ui.status, /状态待确认/); assert.equal(ui.membershipUncertain, true); assert.equal(ui.applied, undefined);
  ui = harness(); subscription.refreshRequired = true;
  ui.manageMembership(); await ui.pending;
  assert.match(ui.status, /状态待确认/); assert.equal(ui.vipActive, false);

  for (const phase of ['manage', 'confirm']) {
    ui = harness();
    const changeAccount = async () => { owner = 'bob'; };
    if (phase === 'manage') manageAction = changeAccount; else confirmAction = changeAccount;
    ui.manageMembership(); await ui.pending;
    assert.equal(ui.applied, undefined, 'old account result cannot reach the new account');
    if (phase === 'manage') assert.equal(confirmCalls, 0);
  }
  ui = harness(); confirmAction = async () => { ui.alive = false; };
  ui.manageMembership(); await ui.pending; assert.equal(ui.applied, undefined, 'discard results for closed pages');

  ui = harness(); restoreError = new Error('INVALID_SUBSCRIPTION');
  ui.restoreMembership(); await ui.pending;
  assert.equal(restoreCalls, 1); assert.equal(confirmCalls, 1); assert.equal(ui.vipActive, true);
  assert.match(ui.status, /当前会员已核验，部分历史购买待重试/);
  assert.equal(ui.membershipAccepted, false, 'restoration needs no checkout consent');
  ui = harness(); ui.restoreMembership(); await ui.pending;
  assert.equal(ui.status, '恢复购买核验完成');
  ui = harness(); subscription.active = false; restoreError = new Error('INVALID_SUBSCRIPTION');
  ui.restoreMembership(); await ui.pending;
  assert.match(ui.status, /部分历史购买尚未恢复/); assert.equal(ui.vipActive, false);
  ui = harness(); confirmError = new Error('PROVIDER_UNAVAILABLE');
  ui.restoreMembership(); await ui.pending; assert.match(ui.status, /状态待确认/); assert.equal(ui.membershipUncertain, true);
  ui = harness(); restoreAction = async () => { owner = 'bob'; };
  ui.restoreMembership(); await ui.pending; assert.equal(confirmCalls, 0); assert.equal(ui.applied, undefined);
  console.log('Membership checkout UI: explicit consent, reminder preflight, visible failures, SDK error code, pending payment, cancellation, retry, duplicate prevention, account isolation and product changes PASS');
  console.log('Subscription management: fresh official state, cancellation keeps access, dialog dismissal, partial recovery, query failures and account/page isolation PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
