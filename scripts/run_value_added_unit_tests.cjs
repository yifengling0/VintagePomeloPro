#!/usr/bin/env node
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const dir = process.argv[2];
if (!dir) throw new Error('compiled-model-dir required');
const m = require(path.resolve(dir, 'model/ValueAddedModels.js'));
const app = require(path.resolve(dir, 'model/AppModels.js'));
for (const type of ['tablet', 'pc', '2in1']) assert.equal(m.isValueAddedVisibleOnDevice(type), true);
for (const type of ['phone', 'tv', '', 'watch', 'unknown', 'TABLET']) {
  assert.equal(m.isValueAddedVisibleOnDevice(type), false);
  assert.throws(() => m.profileDeviceClass(type), /FEATURE_HIDDEN/);
}
assert.equal(m.profileDeviceClass('pc'), 'desktop');
assert.equal(m.profileDeviceClass('2in1'), 'desktop');
assert.equal(m.profileDeviceClass('tablet'), 'tablet');
const config = { apiBaseUrl: 'https://api.example.com', accountClientId: 'client-id',
  membershipProductType: 3, membershipProductId: 'member', coffeeProductId: 'coffee' };
m.validateValueAddedConfig(config);
for (const url of ['', 'http://api.example.com', 'https://user:password@api.example.com',
  'https://api.example.com?token=x', 'https://api.example.com/#x']) {
  assert.throws(() => m.validateValueAddedConfig({ ...config, apiBaseUrl: url }), /NOT_CONFIGURED/);
}
assert.throws(() => m.validateValueAddedConfig({ ...config, coffeeProductId: 'member' }), /NOT_CONFIGURED/);
for (const p of app.createBuiltInInputProfiles()) {
  const payload = m.cloudProfilePayload(p);
  assert.deepEqual(JSON.parse(JSON.stringify(payload)), JSON.parse(JSON.stringify(p)));
  assert.deepEqual(m.validateCloudProfile(JSON.parse(JSON.stringify(payload))), payload);
  for (const field of ['elementsPhone', 'elementsDesktop', 'elementsTablet', 'token', 'libraryPath', 'password']) {
    assert.throws(() => m.validateCloudProfile({ ...payload, [field]: [] }), /INVALID_PROFILE/);
  }
  for (const field of ['path', 'uri', 'secret']) {
    const invalid = structuredClone(payload); invalid.elements[0][field] = '/private';
    assert.throws(() => m.validateCloudProfile(invalid), /INVALID_PROFILE/);
  }
}
const p = app.createDefaultInputProfile();
p.elements[0].widthScale = 1.234;
p.elements[0].heightScale = 3.5;
p.elements[0].keyWidthRatio = 0.037;
p.elements[0].panelWidthRatio = 0.9;
p.elements[0].panelHeightRatio = 0.3;
p.elements[0].keyHeightRatio = 0.1;
assert.deepEqual(m.cloudProfilePayload(p), JSON.parse(JSON.stringify(p)));
assert.throws(() => m.validateCloudProfile({ ...p, schemaVersion: 5 }), /INVALID_PROFILE/);
assert.throws(() => m.validateCloudProfile({ ...p, id: '../../secret' }), /INVALID_PROFILE/);
assert.throws(() => m.validateCloudProfile({ ...p, elements: [p.elements[0], p.elements[0]] }), /INVALID_PROFILE/);
for (const unsafe of ['', '/absolute', '../save', 'a/../b', 'C:/save', 'a\\b', 'a//b', 'a\0b', 'a/', 'a./b', 'a /b']) {
  assert.throws(() => m.safeRelativeSavePath(unsafe), /UNSAFE_SAVE_PATH/);
}
assert.equal(m.safeRelativeSavePath('Saved Games/slot01.sav'), 'Saved Games/slot01.sav');
const entry = { relativePath: 'save/a.sav', size: 10, modifiedAt: 1, sha256: 'a'.repeat(64) };
m.validateSaveEntries([entry]);
assert.throws(() => m.validateSaveEntries([entry, { ...entry, relativePath: 'SAVE/A.sav' }]), /SAVE_PATH_COLLISION/);
assert.throws(() => m.validateSaveEntries([{ ...entry, size: 67108865 }]), /INVALID_SAVE_MANIFEST/);
assert.throws(() => m.validateSaveEntries([]), /SAVE_QUOTA/);
console.log('Value-added models: device matrix, config, full v6 round-trip, allowlist, paths, quotas PASS');

const manifest = fs.readFileSync(path.join(__dirname, '../entry/src/main/module.json5'), 'utf8');
const devices = manifest.match(/"deviceTypes"\s*:\s*\[([\s\S]*?)\]/)[1];
for (const type of ['phone', 'tablet', '2in1']) assert.ok(devices.includes(JSON.stringify(type)), 'Retain platform package: ' + type);
