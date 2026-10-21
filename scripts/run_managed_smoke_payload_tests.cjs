#!/usr/bin/env node
// Actual payload synchronization against isolated host filesystem fixtures.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'managed-smoke-'));
const base = '/data/storage/el2/base/files';
const map = p => {
  assert.ok(p.startsWith(base + '/'), p);
  return path.join(fixture, p.slice(base.length));
};
const put = (p, data) => {
  fs.mkdirSync(path.dirname(map(p)), { recursive: true });
  fs.writeFileSync(map(p), data);
};
const read = p => fs.readFileSync(map(p), 'utf8');
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/ManagedSmokePayloadService.ets'), 'utf8');
const io = {
  accessSync: p => fs.existsSync(map(p)),
  mkdirSync: p => fs.mkdirSync(map(p), { recursive: true }),
  lstatSync: p => fs.lstatSync(map(p)), statSync: p => fs.statSync(map(p)),
  unlinkSync: p => fs.unlinkSync(map(p)), rmdirSync: p => fs.rmdirSync(map(p)),
  listFileSync: p => fs.readdirSync(map(p)),
  copyFileSync: (a, b) => fs.copyFileSync(map(a), map(b)),
  readTextSync: p => read(p)
};
const moduleObject = { exports: {} };
vm.runInNewContext(ts.transpileModule(source, { compilerOptions: {
  module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020
}}).outputText, {
  module: moduleObject, exports: moduleObject.exports,
  require: name => {
    if (name === '@kit.CoreFileKit') return { fileIo: io };
    if (name === './LogService') return { hilog: { info(){}, warn(){}, error(){} } };
    throw new Error(name);
  }
});
const sync = moduleObject.exports.syncManagedSmoke;
const runtime = base + '/wine/smoke';
const diagnostic = base + '/smoke-payload';
const target = base + '/.wine/drive_c/smoke';
const binaries = ['winehua_audio_smoke.exe', 'winehua_graphics_smoke.exe',
  'winehua_vulkan_smoke.exe', 'winehua_d3d11_smoke.exe', 'winehua_d3d8_smoke.exe',
  'winehua_d3d_switch_cube.exe', 'winehua_gpu_diagnostics.exe',
  'winehua_dxvk26_requirements.exe', 'winehua_win32_driver.exe'];
try {
  put(runtime + '/manifest.json', 'runtime-v1');
  put(runtime + '/suites.json', '{"suites":{"runtime":{}}}');
  for (const arch of ['x64', 'x86']) {
    for (const name of binaries) put(runtime + '/' + arch + '/' + name, 'runtime-' + arch);
  }
  put(runtime + '/x64/winehua_d3d12_smoke.exe', 'd3d12');
  for (const name of ['triangle.exe', 'gears.exe']) put(runtime + '/x64/' + name, name);
  put(runtime + '/assets/texture.bin', 'texture');
  const catalog = JSON.parse(fs.readFileSync(path.join(__dirname,
    '../entry/src/main/resources/rawfile/builtin_apps/catalog.json'), 'utf8'));
  assert.equal(catalog.apps.find(a => a.id === 'builtin-graphics-smoke').launchParams.executable,
    'C:\\smoke\\x64\\winehua_graphics_smoke.exe');
  for (const app of catalog.apps.filter(a => a.launchParams.executable.startsWith('C:\\smoke\\'))) {
    const relative = app.launchParams.executable.slice('C:\\smoke\\'.length).replaceAll('\\', '/');
    assert.ok(fs.existsSync(map(runtime + '/' + relative)), app.id + ' packaged executable');
  }

  put(diagnostic + '/manifest.json', 'media-only-v1');
  put(diagnostic + '/suites.json', '{"suites":{"media":{}}}');
  put(diagnostic + '/x64-fex/media.exe', 'media');
  put(target + '/manifest.json', 'media-only-v1');
  put(target + '/suites.json', read(diagnostic + '/suites.json'));
  put(target + '/x64-fex/media.exe', 'existing-media');
  put(target + '/results/previous/result.json', 'retained-result');
  put(target + '/x64/d3d11.dll', 'stale-overlay');
  put(base + '/user-games/save.dat', 'user-save');

  assert.equal(sync('reuse'), true, 'default launch repairs partial diagnostic installation');
  assert.equal(read(target + '/x64/winehua_d3d_switch_cube.exe'), 'runtime-x64');
  assert.equal(read(target + '/x86/winehua_d3d_switch_cube.exe'), 'runtime-x86');
  assert.equal(read(target + '/x64/triangle.exe'), 'triangle.exe');
  assert.equal(read(target + '/manifest.json'), 'runtime-v1');
  assert.equal(read(target + '/x64-fex/media.exe'), 'existing-media');
  assert.equal(read(target + '/results/previous/result.json'), 'retained-result');
  assert.equal(fs.existsSync(map(target + '/x64/d3d11.dll')), false);

  fs.unlinkSync(map(target + '/x64/winehua_d3d_switch_cube.exe'));
  assert.equal(sync('reuse'), true, 'matching manifest alone does not hide a missing executable');
  assert.equal(read(target + '/x64/winehua_d3d_switch_cube.exe'), 'runtime-x64');
  fs.unlinkSync(map(target + '/x64/gears.exe'));
  assert.equal(sync('reuse'), true, 'optional packaged demos are repaired');
  assert.equal(read(target + '/x64/gears.exe'), 'gears.exe');

  assert.equal(sync('reuse', 'diagnostic'), true, 'diagnostic suite explicitly selects its payload');
  assert.equal(read(target + '/manifest.json'), 'media-only-v1');
  assert.equal(read(target + '/x64-fex/media.exe'), 'media');
  assert.equal(read(target + '/results/previous/result.json'), 'retained-result');
  assert.equal(sync('reuse'), true, 'built-ins recover after a diagnostic suite');
  assert.equal(read(target + '/x64/winehua_d3d_switch_cube.exe'), 'runtime-x64');
  assert.equal(read(target + '/results/previous/result.json'), 'retained-result');
  assert.equal(read(base + '/user-games/save.dat'), 'user-save');

  assert.equal(sync('clean'), true, 'clean prefix remains isolated');
  assert.equal(read(base + '/.wine-smoke/drive_c/smoke/x64/winehua_d3d_switch_cube.exe'), 'runtime-x64');
  fs.unlinkSync(map(runtime + '/x64/winehua_d3d_switch_cube.exe'));
  put(runtime + '/manifest.json', 'runtime-v2');
  assert.equal(sync('reuse'), false, 'incomplete runtime reports failure');
  assert.equal(read(target + '/results/previous/result.json'), 'retained-result');
  console.log('Managed smoke: partial diagnostics, interrupted extraction, demos, source switching, result preservation, isolated prefix, missing runtime PASS');
} finally {
  fs.rmSync(fixture, { recursive: true, force: true });
}
