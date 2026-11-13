const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));

function load(file, deps) {
  const exports = {};
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets', file), 'utf8');
  const compiled = ts.transpileModule(source, { compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  } }).outputText;
  vm.runInNewContext(compiled, { exports, Set, Map, Promise, Date, JSON, Number, Math,
    require(id) { assert.ok(id in deps, `unexpected dependency ${id}`); return deps[id]; } });
  return exports;
}

const { MembershipCampaigns } = load('model/MembershipCampaign.ets', {
  '@kit.LocalizationKit': {
    intl: {
      NumberFormat: class {
        constructor(_locale, options) { this.currency = options.currency; }
        format(value) { return this.currency + ' ' + value.toFixed(2); }
      }
    }
  }
});

const file = JSON.stringify({ schema: 1, campaigns: [{
  productId: 'VIP', offerId: 'time_limit', currency: 'CNY',
  originalMicroPrice: 68000000, microPrice: 38000000,
  endsAt: '2027-01-01T00:00:00+08:00'
}] });
const campaigns = MembershipCampaigns.parse(file);
assert.equal(campaigns.length, 1);
const before = Date.parse('2026-11-07T00:00:00+08:00');
assert.equal(MembershipCampaigns.active(campaigns, 'VIP', 'CNY', before).offerId, 'time_limit');
assert.equal(MembershipCampaigns.active(campaigns, 'VIP', 'USD', before), undefined);
assert.equal(MembershipCampaigns.active(campaigns, 'OTHER', 'CNY', before), undefined);
assert.equal(MembershipCampaigns.deadline(campaigns[0]), '限时优惠至 2026-12-31 23:59（北京时间）');
assert.equal(MembershipCampaigns.money(38000000, 'CNY'), 'CNY 38.00');
const after = Date.parse('2027-01-01T00:00:00+08:00');
assert.equal(MembershipCampaigns.active(campaigns, 'VIP', 'CNY', after), undefined);

for (const invalid of [
  '{}',
  JSON.stringify({ schema: 1, campaigns: [{ productId: 'VIP', offerId: 'bad id', currency: 'CNY',
    originalMicroPrice: 100, microPrice: 50, endsAt: '2027-01-01T00:00:00+08:00' }] }),
  JSON.stringify({ schema: 1, campaigns: [{ productId: 'VIP', offerId: 'offer', currency: 'CNY',
    originalMicroPrice: 50, microPrice: 50, endsAt: '2027-01-01T00:00:00+08:00' }] }),
  JSON.stringify({ schema: 1, campaigns: [{ productId: 'VIP', offerId: 'offer', currency: 'CNY',
    originalMicroPrice: 100, microPrice: 50, endsAt: 'not-a-date' }] }),
  JSON.stringify({ schema: 1, campaigns: [
    { productId: 'VIP', offerId: 'a', currency: 'CNY', originalMicroPrice: 100, microPrice: 50,
      endsAt: '2027-01-01T00:00:00+08:00' },
    { productId: 'VIP', offerId: 'b', currency: 'CNY', originalMicroPrice: 100, microPrice: 60,
      endsAt: '2027-01-01T00:00:00+08:00' }
  ] })
]) {
  assert.throws(() => MembershipCampaigns.parse(invalid));
}
console.log('Membership campaign: parse, active window, deadline and invalid configuration PASS');
