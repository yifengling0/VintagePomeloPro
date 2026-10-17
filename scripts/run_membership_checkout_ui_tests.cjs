#!/usr/bin/env node
// Run the component's real checkout methods without ArkUI rendering or a payment SDK.
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(process.argv[2]);
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/components/AccountSyncSettings.ets'), 'utf8');
const names = ['buy', 'canBuyMembership', 'clearConfirmation', 'refreshReminder', 'openCheckout', 'selectProduct', 'checkoutProduct'];
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
    cashierCalls++; return 'CANCELLED';
  }
};
const js = ts.transpileModule('class Harness {\n' + methods + '\n}\nmodule.exports = Harness;',
  { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
const mod = { exports: {} };
vm.runInNewContext(js, { module: mod, ValueAddedService: { getInstance: () => service },
  MEMBERSHIP_TERMS_VERSION: '2026-10-16', AccountAutomaticSyncService: { getInstance: () => ({ requestSync() {} }) } });
function harness() {
  owner = 'alice'; capabilities = { commerce: true, membershipPurchases: true, renewalReminders: true };
  ready = true; enabled = true; permissionCalls = []; cashierCalls = 0; refreshCalls = 0; permissionAction = null;
  const ui = Object.assign(new mod.exports(), { alive: true, busy: false, userId: owner, commerceReady: true,
    membershipAccepted: false, autoRenewAccepted: false, selectedProductId: product.id, reminderReady: false,
    showCheckout: true, products: [{ ...product, purpose: 'membership' }], vipActive: false, membershipUncertain: false });
  ui.errorMessage = e => e.message;
  ui.refreshAccount = async () => {};
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

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; ready = false; enabled = false;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0); assert.match(ui.status, /通知/); assert.equal(ui.membershipAccepted, true);
  ready = true; enabled = true; ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 1, 'granting permission after denial permits retry');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true; capabilities.renewalReminders = false;
  ui.buy(product); await ui.pending;
  assert.equal(cashierCalls, 0); assert.equal(permissionCalls.length, 0); assert.equal(ui.status, 'REMINDERS_UNAVAILABLE');

  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  permissionAction = async () => { owner = 'bob'; ui.userId = owner; ui.clearConfirmation(); };
  ui.buy(product); await ui.pending; assert.equal(cashierCalls, 0, 'account change during permission cannot pay');

  ui = harness(); ui.membershipAccepted = true; ui.selectedProductId = 'once';
  ui.buy({ id: 'once', type: 3, price: '¥5.00' }); await ui.pending;
  assert.equal(cashierCalls, 1); assert.equal(permissionCalls.length, 0, 'single month requires no renewal permission');
  ui = harness(); ui.membershipAccepted = ui.autoRenewAccepted = true;
  ui.selectProduct({ id: 'once', type: 3, price: '¥5.00' });
  assert.equal(ui.showCheckout, false); assert.equal(ui.membershipAccepted, false); assert.equal(ui.autoRenewAccepted, false);
  console.log('Membership checkout UI: confirmation sheet, explicit consent, permission preflight, denied retry, backend outage, account change, product change and single month PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
