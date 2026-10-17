#!/usr/bin/env node
// Run the component's real checkout methods without ArkUI rendering or a payment SDK.
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(process.argv[2]);
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/components/AccountSyncSettings.ets'), 'utf8');
const names = ['buy', 'showPurchaseMessage', 'canBuyMembership', 'clearConfirmation', 'refreshReminder', 'openCheckout', 'selectProduct', 'checkoutProduct'];
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
const prompt = {
  showDialog: async options => { dialogCalls.push(options); if (dialogAction) await dialogAction(); },
  showToast: options => toastCalls.push(options)
};
const service = {
  getUserId: () => owner, getCapabilities: () => capabilities, getMembershipOperator: () => 'Publisher',
  quoteKey: p => p.id + ':' + p.price,
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
  MEMBERSHIP_TERMS_VERSION: '2026-10-16', AccountAutomaticSyncService: { getInstance: () => ({ requestSync() {} }) } });
function harness() {
  owner = 'alice'; capabilities = { commerce: true, membershipPurchases: true, renewalReminders: true };
  ready = true; enabled = true; permissionCalls = []; cashierCalls = 0; refreshCalls = 0; permissionAction = null;
  purchaseError = null; purchaseResult = 'CANCELLED'; dialogCalls = []; toastCalls = []; dialogAction = null; accountError = null;
  const ui = Object.assign(new mod.exports(), { alive: true, busy: false, userId: owner, commerceReady: true,
    membershipAccepted: false, autoRenewAccepted: false, selectedProductId: product.id, reminderReady: false,
    showCheckout: true, products: [{ ...product, purpose: 'membership' }], vipActive: false, membershipUncertain: false });
  ui.errorMessage = e => e.message;
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
  assert.match(dialogCalls[0].message, /REMINDERS_UNAVAILABLE/); assert.match(ui.status, /未发起支付，没有扣款/);
  assert.equal(ui.busy, false, 'dismissal permits a later retry');

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
  assert.match(dialogCalls[0].message, /PRICE_CHANGED/); assert.equal(ui.showCheckout, false);

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
  console.log('Membership checkout UI: explicit consent, reminder preflight, visible failures, SDK error code, pending payment, cancellation, retry, duplicate prevention, account isolation and product changes PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
