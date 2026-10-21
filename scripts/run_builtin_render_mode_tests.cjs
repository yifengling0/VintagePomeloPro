#!/usr/bin/env node
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const root = path.resolve(__dirname, '..');
const moduleObject = { exports: {} };
const js = ts.transpileModule(fs.readFileSync(path.join(root,
  'entry/src/main/ets/model/AppCatalogRules.ets'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
}).outputText;
vm.runInNewContext(js, { module: moduleObject, exports: moduleObject.exports });
const supports = moduleObject.exports.builtinSupportsRenderMode;
const catalog = JSON.parse(fs.readFileSync(path.join(root,
  'entry/src/main/resources/rawfile/builtin_apps/catalog.json'), 'utf8'));
for (const mode of ['dxvk_1_10', 'dxvk_2_6', 'virgl']) {
  const visible = catalog.apps.filter(a => supports(mode, a.renderModes, a.enabled)).map(a => a.id);
  assert.equal(visible.includes('builtin-dxvk-cube'), mode !== 'virgl');
  // This branch archives VKD3D; selecting DXVK 2.6 does not enable D3D12.
  for (const id of ['builtin-vkd3d-triangle', 'builtin-vkd3d-gears']) assert.equal(visible.includes(id), false);
  for (const id of ['builtin-graphics-smoke', 'builtin-audio-smoke', 'builtin-notepad', 'builtin-winecfg'])
    assert.equal(visible.includes(id), true);
}
assert.equal(supports('dxvk_1_10', ['dxvk_2_6']), false);
assert.equal(supports('dxvk_2_6', ['dxvk_2_6']), true);
assert.equal(supports('virgl', ['dxvk_2_6']), false);
assert.equal(supports('dxvk_2_6', []), false);
assert.equal(supports('dxvk_2_6', undefined, false), false);
assert.equal(supports('dxvk_1_10'), true);
console.log('Built-in catalog: DXVK 1.10 / 2.6 / VirGL, modern-only restrictions, archived VKD3D and common tools PASS');
