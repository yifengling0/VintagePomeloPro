// Exercise the production Windows-only client owner, with launch I/O stubbed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const state = {installed: false, importing: false, running: false, result: {}, failure: false};
const calls = [], readiness = [];
let returns = 0;
let gate = null;
let navigation = 1;
let windowKey = '';
let activeKeys = [];
let activationResult = true;
let navigateDuringActivation = false;
const singleton = value => ({getInstance: () => value});
const windows = {isInstalled: () => state.installed, isImporting: () => state.importing,
  hasRunningProcesses: () => state.running,
  clientCard: () => ({steam: {state: 'client'}, launchTarget: {
    executable: '/download/Steam/steam.exe', workingDirectory: '/download/Steam'}})};
const deps = {
  '../steam/SteamLocalService': {SteamLocalService: singleton(windows)},
  './AppCatalogService': {AppCatalogService: singleton({getWineHomeDirectory: () => '/download'})},
  './AppSettingsStore': {AppSettingsStore: singleton({getGlobalSettings: () => ({displayMode: 'desktop'})})},
  './HostWindowTasks': {HostWindowTasks: singleton({
    navigationSerial: () => navigation,
    showLauncher: async () => { ++returns; },
    async activate(key) {
      activeKeys.push(key);
      ++navigation;
      if (navigateDuringActivation) queueMicrotask(() => { ++navigation; });
      return activationResult;
    }
  })},
  './AppSessionService': {AppSessionService: singleton({steamClientWindowKey: () => windowKey, async launch(app, settings, root) {
    calls.push([app, settings, root]); if (gate) await gate;
    if (state.failure) throw Error('wine failure'); return state.result;
  }, async waitForSteamClientWindow(app, result, navigation) {
    readiness.push([app, result, navigation]);
    if (state.readyFailure) throw Error('client window missing');
  }})}
};
const out = {};
const file = path.resolve(__dirname, '../entry/src/main/ets/service/SteamClientService.ets');
vm.runInNewContext(ts.transpileModule(fs.readFileSync(file, 'utf8'), {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021}
}).outputText, {exports: out, Promise, Error, require(name) {
  assert.ok(name in deps, `unexpected runtime dependency ${name}`); return deps[name];
}});
const service = new out.SteamClientService();
(async () => {
  assert.equal(service.installed(), false);
  await assert.rejects(service.open(), /尚未安装/); assert.equal(calls.length, 0);
  state.installed = true;
  assert.equal(service.installed(), true);
  for (const action of ['library', 'downloads', 'account', 'store', 'bigPicture', 'desktop']) {
    await service.open(action);
    assert.equal(calls.at(-1)[0].steam.clientAction, action);
    assert.equal(calls.at(-1)[0].launchTarget.executable, '/download/Steam/steam.exe');
    assert.equal(calls.at(-1)[2], '/download');
  }
  state.running = true; assert.equal(service.isRunning(), true);
  await service.open(); assert.equal(calls.at(-1)[0].steam.clientAction, 'library', 'warm navigation reaches the existing session owner');
  windowKey = 'steam-window';
  const beforeRestore = calls.length;
  await service.open('library', true);
  assert.deepEqual(activeKeys, ['steam-window']);
  assert.equal(calls.length, beforeRestore, 'Show Steam restores the known window without starting a command sender');
  activationResult = false;
  navigateDuringActivation = true;
  await service.open('library', true);
  assert.equal(calls.length, beforeRestore, 'A later navigation cancels restoration without a second launch');
  navigateDuringActivation = false;
  await service.open('library', true);
  assert.equal(calls.length, beforeRestore + 1, 'A window that disappears falls back to library navigation');
  windowKey = '';
  await service.open('library', true);
  assert.equal(calls.length, beforeRestore + 2, 'A silent client still receives a library command');
  state.running = false; assert.equal(service.isRunning(), false);
  const before = calls.length;
  state.importing = true;
  await assert.rejects(service.open(), /正在导入或更新/);
  state.installed = false;
  await assert.rejects(service.open(), /正在导入或更新/, 'partial import must not become a false missing-install result');
  state.importing = false;
  await assert.rejects(service.open(), /尚未安装/, 'installation is checked on each request');
  state.installed = true;
  for (const action of ['launch', 'shutdown', 'linux', '../games']) {
    await assert.rejects(service.open(action), /客户端操作无效/);
  }
  assert.equal(calls.length, before, 'blocked requests never launch or terminate a process');
  let release; gate = new Promise(resolve => {release = resolve;});
  const opening = service.open('downloads');
  await assert.rejects(service.open(), /正在启动/);
  release(); await opening; gate = null;
  state.failure = true; await assert.rejects(service.open(), /wine failure/);
  state.failure = false; state.result = null;
  await assert.rejects(service.open(), /Windows Steam 启动失败/);
  state.result = {}; await service.open();
  assert.equal(readiness.at(-1)[1], state.result);
  state.readyFailure = true;
  const previous = returns;
  await assert.rejects(service.open(), /client window missing/);
  assert.equal(returns, previous + 1, 'A PID without a ready client returns to launcher and reports failure');
  state.readyFailure = false; await service.open();
  console.log('Windows Steam owner: empty/importing/stale install, navigation, original descriptor, warm session, concurrent starts and failure retry PASS');
})().catch(error => {console.error(error); process.exitCode = 1;});
