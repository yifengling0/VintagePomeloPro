// Exercise the production chooser/router without downloading clients or changing user libraries.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const state = { windows: false, linux: false, importing: false, phase: 'ready',
  linuxActive: false, windowsActive: false, foreign: false, launchResult: {}, fail: false };
const calls = [];
let gate = null;
const singleton = instance => ({ getInstance: () => instance });
const windows = { isInstalled: () => state.windows, isImporting: () => state.importing,
  hasRunningProcesses: () => state.windowsActive, hasForeignClient: () => state.foreign,
  clientCard: () => ({ steam: { appId: '', state: 'client' },
    launchTarget: { executable: '/download/Steam/steam.exe', workingDirectory: '/download/Steam' } }) };
const linux = { isActive: () => state.linuxActive, initialize: context => calls.push(['initialize', context]),
  async open(action) { calls.push(['linux', action]); if (gate) await gate; if (state.fail) throw Error('native failure'); } };
const deps = {
  '../steam/SteamLocalService': { SteamLocalService: singleton(windows) },
  './LinuxSteamInstallService': { LinuxSteamInstallService: singleton({ installed: () => state.linux,
    snapshot: () => ({ phase: state.phase }) }) },
  './LinuxSteamSessionService': { LinuxSteamSessionService: singleton(linux) },
  './AppCatalogService': { AppCatalogService: singleton({ getWineHomeDirectory: () => '/download' }) },
  './AppSettingsStore': { AppSettingsStore: singleton({ getGlobalSettings: () => ({ displayMode: 'desktop' }) }) },
  './AppSessionService': { AppSessionService: singleton({ async launch(app, settings, root) {
    calls.push(['windows', app, settings, root]); if (gate) await gate;
    if (state.fail) throw Error('wine failure'); return state.launchResult;
  } }) },
  '../model/LinuxSteamDownloadModels': { linuxSteamInstallBusy: phase => phase === 'downloading' }
};
const exportsObject = {};
const file = path.resolve(__dirname, '../entry/src/main/ets/service/SteamClientService.ets');
vm.runInNewContext(ts.transpileModule(fs.readFileSync(file, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021 }
}).outputText, { exports: exportsObject, Promise, Error, require(name) {
  assert.ok(name in deps, `unexpected runtime dependency ${name}`); return deps[name];
} });
const service = new exportsObject.SteamClientService();
let dialog = null, dialogs = 0;
const ui = { showAlertDialog(options) { dialogs++; dialog = options; } };
const context = { id: 'ability' };
(async () => {
  assert.equal(await service.choose(ui), ''); assert.equal(dialogs, 0, 'none installed never opens a chooser');
  await assert.rejects(service.open(context, 'windows'), /尚未安装/);
  state.windows = true;
  assert.equal(await service.choose(ui), 'windows'); assert.equal(dialogs, 0);
  await service.open(context, 'windows');
  assert.equal(calls.at(-1)[0], 'windows'); assert.equal(calls.at(-1)[1].steam.clientAction, 'library');
  assert.equal(calls.at(-1)[1].launchTarget.executable, '/download/Steam/steam.exe');
  assert.equal(calls.at(-1)[3], '/download');
  state.windows = false; state.linux = true;
  assert.equal(await service.choose(ui), 'linux'); assert.equal(dialogs, 0);
  await service.open(context, 'linux', 'bigPicture'); assert.equal(calls.at(-1)[0], 'linux');
  assert.equal(calls.at(-1)[1], 'bigPicture');
  state.windows = true;
  const first = service.choose(ui), second = service.choose(ui);
  assert.equal(dialogs, 1, 'repeated clicks share one selector'); dialog.buttons[0].action();
  assert.equal(await first, 'windows'); assert.equal(await second, 'windows');
  const next = service.choose(ui); dialog.buttons[1].action(); assert.equal(await next, 'linux');
  assert.equal(dialogs, 2, 'both clients stay selectable on subsequent launches');
  const cancelled = service.choose(ui); dialog.cancel(); assert.equal(await cancelled, '');
  const cancelButton = service.choose(ui); dialog.buttons[2].action(); assert.equal(await cancelButton, '');
  const stale = service.choose(ui); state.windows = false; dialog.buttons[0].action();
  await assert.rejects(service.open(context, await stale), /尚未安装/, 'installation rechecked after user selects');
  assert.equal(await service.choose(ui), 'linux', 'remaining installation becomes the direct default');
  state.windows = true;
  for (const action of ['library', 'downloads', 'account', 'store', 'bigPicture', 'desktop']) {
    await service.open(context, 'windows', action);
    assert.equal(calls.at(-1)[0], 'windows'); assert.equal(calls.at(-1)[1].steam.clientAction, action);
  }
  const before = calls.length;
  state.linuxActive = true;
  await assert.rejects(service.open(context, 'windows'), /先正常退出 Linux/);
  state.linuxActive = false; state.windowsActive = true;
  await assert.rejects(service.open(context, 'linux'), /先正常退出 Windows/);
  state.windowsActive = false; state.foreign = true;
  await assert.rejects(service.open(context, 'linux'), /先正常退出 Windows/);
  state.foreign = false; state.importing = true;
  await assert.rejects(service.open(context, 'linux'), /正在安装或更新/);
  state.importing = false; state.phase = 'downloading';
  await assert.rejects(service.open(context, 'windows'), /正在安装或更新/);
  state.phase = 'ready';
  await assert.rejects(service.open(context, 'windows', 'launch'), /对应的游戏/);
  await assert.rejects(service.open(context, 'linux', 'shutdown'), /对应的游戏/);
  assert.equal(calls.length, before, 'blocked requests never launch or terminate either client');
  let release; gate = new Promise(resolve => { release = resolve; });
  const opening = service.open(context, 'windows');
  await assert.rejects(service.open(context, 'linux'), /另一个 Steam 正在启动/);
  await assert.rejects(service.open(context, 'windows'), /正在启动/);
  release(); await opening; gate = null;
  state.fail = true; await assert.rejects(service.open(context, 'linux'), /native failure/);
  await assert.rejects(service.open(context, 'windows'), /wine failure/);
  state.fail = false; state.launchResult = null;
  await assert.rejects(service.open(context, 'windows'), /Windows Steam 启动失败/);
  state.launchResult = {}; await service.open(context, 'windows');
  console.log('Steam client routing PASS: none/single/both, cancellation, stale install, actions, backend isolation, concurrent starts and failure recovery');
})().catch(error => { console.error(error); process.exitCode = 1; });
