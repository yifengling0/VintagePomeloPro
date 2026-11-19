const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(process.argv[2]);
const ts = require(path.resolve(process.argv[3]));
const models = require(path.join(root, 'model/AppModels.js'));
const rules = require(path.join(root, 'model/GameModeSettings.js'));
assert.deepEqual(rules.normalizeGameModeSettings({ scaling: 'bad', frameLimit: 120 }), { scaling: 'linear', frameLimit: 0, sharpness: 10, frameGeneration: 'off' });
assert.deepEqual(rules.normalizeGameModeSettings({ scaling: 'fsr', frameLimit: 60, sharpness: 0 }), { scaling: 'fsr', frameLimit: 60, sharpness: 0, frameGeneration: 'off' });
assert.equal(rules.normalizeGameModeSettings({ scaling: 'fsr', sharpness: NaN }).sharpness, 10);
assert.equal(rules.normalizeGameModeSettings({ scaling: 'fsr', sharpness: 21 }).sharpness, 10);
assert.equal(rules.sameGamePresentation({ enabled: true, settings: { scaling: 'fsr', frameLimit: 30, sharpness: 0 } },
  { enabled: true, settings: { scaling: 'fsr', frameLimit: 30, sharpness: 20 } }), false);
assert.equal(rules.resolveGamePresentation(true, 'off').enabled, false);
assert.equal(rules.resolveGamePresentation(false, 'on').enabled, true);
assert.equal(rules.resolveGamePresentation(true, undefined).enabled, true);
assert.equal(rules.resolveGamePresentation(false, undefined).enabled, false);
assert.deepEqual(rules.resolveGamePresentation(true, 'follow', { scaling: 'linear', frameLimit: 60 },
  { scaling: 'integer', frameLimit: 30 }).settings, { scaling: 'integer', frameLimit: 30, sharpness: 10, frameGeneration: 'off' });
const clone = models.cloneAppSettings({ ...models.createDefaultAppSettings('game'),
  gameModeOverride: 'on', gameModeSettings: { scaling: 'nearest', frameLimit: 30 } });
assert.equal(clone.gameModeOverride, 'on');
clone.gameModeSettings.frameLimit = 60;

const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/GameModeService.ets'), 'utf8');
const code = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
function fixture() {
  const values = new Map(), calls = [], targets = [], exports = {};
  let appSettings = models.createDefaultAppSettings('game');
  const dependencies = {
    'libentry.so': { default: { configureGamePresentation: (...args) => calls.push(args),
      presentGameToplevel: id => { targets.push(id); return true; } } },
    '../model/AppModels': models, '../model/GameModeSettings': rules,
    './AppSettingsStore': { AppSettingsStore: { getInstance: () => ({
      getAppSettings: () => appSettings, getRenderMode: () => 'dxvk_1_10'
    }) } }
  };
  vm.runInNewContext(code, { exports, require: name => {
    assert.ok(Object.hasOwn(dependencies, name), name); return dependencies[name];
  }, AppStorage: { get: key => values.get(key), setOrCreate: (key, value) => values.set(key, value) } });
  return { service: exports.GameModeService, values, calls, targets, appSettings, dependencies };
}
const app = { id: 'game', source: models.AppSource.DOWNLOAD,
  launchTarget: { kind: models.LaunchKind.WINE_EXE, arguments: [] } };
const global = models.createDefaultGlobalSettings();
global.gameMode = { scaling: 'nearest', frameLimit: 30 };
{
  const f = fixture(); f.values.set('vpp.game.browsing', true);
  assert.equal(f.calls.length, 0); // entering library does not start a presenter
  f.service.begin(app, global, false);
  assert.deepEqual(f.calls[0], [true, 'nearest', 30, 10, 'off']);
  f.service.reset(); // startup restart cannot clear a reserved policy
  assert.equal(f.values.get('vpp.game.session'), true);
  f.service.finish(true);
  assert.equal(f.service.present(42), true);
  assert.equal(f.service.presentDesktop(), true);
  assert.deepEqual(f.targets, [42, 0], 'Steam clears a previous game crop without changing the policy');
  f.service.begin({ ...app, launchTarget: { kind: models.LaunchKind.STEAM_CLIENT } }, global, true); f.service.finish(true);
  f.service.begin({ ...app, launchTarget: { kind: models.LaunchKind.STEAM_GAME } }, global, true); f.service.finish(true);
  f.appSettings.gameModeOverride = 'off';
  assert.throws(() => f.service.begin(app, global, true), /Windows/);
  assert.equal(f.values.get('vpp.game.session'), true);
  f.service.reset(); assert.equal(f.values.get('vpp.game.session'), false);
  assert.equal(f.service.presentDesktop(), false);
  assert.equal(f.targets.length, 2, 'Disabled mode never changes native presentation');
}
{
  const f = fixture(); f.values.set('vpp.game.browsing', true);
  assert.throws(() => f.service.begin(app, global, true), /Windows/);
  f.service.begin(app, global, false); f.service.finish(false);
  assert.equal(f.values.get('vpp.game.session'), false); // failed launch restores
}
console.log('Game mode policy, inheritance, conflict, startup and rollback tests passed');

{
  const f = fixture(); f.values.set('vpp.game.browsing', true);
  f.appSettings.gameModeSettings = { scaling: 'fsr', frameLimit: 60, sharpness: 4 };
  f.service.begin(app, global, false);
  assert.throws(() => f.service.applyLive({ scaling: 'linear', frameLimit: 0 }, f.service.sessionSerial()), /会话已变化/);
  f.service.finish(true);
  assert.deepEqual(f.service.currentSettings(), rules.normalizeGameModeSettings(f.appSettings.gameModeSettings), 'Panel reads the active per-game policy');
  const serial = f.service.sessionSerial();
  f.service.applyLive({ scaling: 'integer', frameLimit: 30 }, serial);
  assert.deepEqual(f.calls.at(-1), [true, 'integer', 30, 10, 'off']);
  assert.equal(f.service.currentSettings().frameLimit, 30);
  assert.deepEqual(global.gameMode, { scaling: 'nearest', frameLimit: 30 }); // global remains untouched
  const publish = f.dependencies['libentry.so'].default.configureGamePresentation;
  f.dependencies['libentry.so'].default.configureGamePresentation = () => { throw Error('native rejected'); };
  assert.throws(() => f.service.applyLive({ scaling: 'linear', frameLimit: 60 }, serial), /native rejected/);
  assert.equal(f.service.currentSettings().scaling, 'integer', 'Rejected native config cannot change the owner');
  f.dependencies['libentry.so'].default.configureGamePresentation = publish;
  f.service.noteActiveApp('steam-child');
  assert.throws(() => f.service.applyLive({ scaling: 'linear', frameLimit: 0 }, serial), /会话已变化/);
  assert.equal(f.service.activeAppId(), 'steam-child');
  f.service.reset();
  assert.throws(() => f.service.applyLive({ scaling: 'linear', frameLimit: 0 }, f.service.sessionSerial()), /会话已变化/);
  assert.equal(models.createDefaultGlobalSettings().gameModeEnabled, false);
  console.log('Live policy, per-game initial value, native failure and stale-panel isolation: PASS');
}

{ // A panel consumes both down/up and cannot be detached by an old instance.
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/GamePanelInput.ets'), 'utf8');
  const code = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
  const exports = {}; vm.runInNewContext(code, { exports });
  const panel = exports.GamePanelInput, received = [];
  const old = key => received.push(['old', key]), current = key => received.push(['new', key]);
  assert.equal(panel.handle(2012, true), false);
  panel.attach(old);
  assert.equal(panel.handle(2012, true), true);
  assert.equal(panel.handle(2012, false), true);
  assert.deepEqual(received, [['old', 2012]]);
  panel.attach(current); panel.detach(old);
  assert.equal(panel.handle(2054, true), true);
  assert.deepEqual(received.at(-1), ['new', 2054]);
  panel.detach(current);
  assert.equal(panel.handle(2054, false), false);
  console.log('Game panel hardware input ownership and key-up isolation passed');
}

{
  const f=fixture(); f.values.set('vpp.game.browsing',true);
  f.appSettings.gameModeSettings={scaling:'linear',frameLimit:30,frameGeneration:'x2'};
  f.service.begin(app,global,false); f.service.finish(true);
  assert.deepEqual(f.calls.at(-1),[true,'linear',30,10,'x2']);
  assert.equal(f.service.currentSettings().frameGeneration,'x2');
  assert.equal(rules.normalizeGameModeSettings().frameGeneration,'off');
  assert.equal(rules.normalizeGameModeSettings({frameGeneration:'invalid'}).frameGeneration,'off');
  assert.equal(rules.sameGamePresentation({enabled:true,settings:{scaling:'linear',frameLimit:30}},
    {enabled:true,settings:{scaling:'linear',frameLimit:30,frameGeneration:'x2'}}),false);
  const copy=models.cloneAppSettings({...models.createDefaultAppSettings('test'),gameModeSettings:f.appSettings.gameModeSettings});
  assert.equal(copy.gameModeSettings.frameGeneration,'x2');
  f.service.applyLive({scaling:'fsr',frameLimit:60,frameGeneration:'off'},f.service.sessionSerial());
  assert.deepEqual(f.calls.at(-1),[true,'fsr',60,10,'off']);
  console.log('Free frame generation: legacy default, per-game clone, native publish and live switch passed');
}

{
  const f = fixture();
  f.values.set('vpp.game.browsing', true);
  f.appSettings.graphicsBackend = 'dxvk9';
  f.appSettings.gameModeSettings = { scaling: 'linear', frameLimit: 0, frameGeneration: 'harmonyGpu' };
  f.service.begin(app, global, false);
  f.service.finish(true);
  assert.deepEqual(f.calls.at(-1), [true, 'linear', 0, 10, 'harmonyGpu']);
  const serial = f.service.sessionSerial();
  const winFg = { scaling: 'fsr', frameLimit: 60, frameGeneration: 'x2' };
  f.service.applyLive(winFg, serial);
  assert.equal(f.service.currentSettings().frameGeneration, 'x2');
  assert.deepEqual(f.calls.at(-1), [true, 'fsr', 60, 10, 'x2']);
  const harmony = { ...winFg, frameGeneration: 'harmonyGpu' };
  f.service.applyLive(harmony, serial);
  assert.deepEqual(f.calls.at(-1), [true, 'fsr', 60, 10, 'harmonyGpu']);
  assert.equal(f.service.currentSettings().frameGeneration, 'harmonyGpu');
  f.service.applyLive({ ...winFg, frameGeneration: 'off' }, serial);
  assert.deepEqual(f.calls.at(-1), [true, 'fsr', 60, 10, 'off']);
  assert.equal(f.service.sessionSerial(), serial);
  console.log('Harmony GPU, Win-FG and off publish within the same Wine session: PASS');
}

{
  const f = fixture();
  f.values.set('vpp.game.browsing', true);
  const harmony = { scaling: 'linear', frameLimit: 0, frameGeneration: 'harmonyGpu' };
  f.appSettings.gameModeSettings = harmony;
  f.appSettings.graphicsBackend = 'virgl';
  f.appSettings.graphicsLaunchPreset = 'dx11';
  assert.match(models.harmonyGpuLaunchBlockReason(app, f.appSettings, 'dxvk_1_10'), /Direct3D 11/);
  f.service.begin(app, global, false); f.service.finish(true);
  assert.equal(f.calls.at(-1)[4], 'off');
  assert.equal(f.appSettings.gameModeSettings.frameGeneration, 'harmonyGpu');
  assert.throws(() => f.service.applyLive(harmony, f.service.sessionSerial()), /Direct3D 11/);
  f.service.applyLive({ ...harmony, frameGeneration: 'x2' }, f.service.sessionSerial());
  assert.equal(f.calls.at(-1)[4], 'x2');

  f.appSettings.graphicsLaunchPreset = 'default';
  f.appSettings.arguments = ['-video_app', 'OpenGL'];
  assert.match(models.harmonyGpuLaunchBlockReason(app, f.appSettings, 'dxvk_1_10'), /OpenGL/);
  f.appSettings.arguments = ['-video_app', 'direct3d9'];
  assert.equal(models.harmonyGpuLaunchBlockReason(app, f.appSettings, 'dxvk_1_10'), '');
  assert.match(models.harmonyGpuLaunchBlockReason(app, { ...f.appSettings, graphicsBackend: 'default' },
    'zink'), /渲染后端/);
  f.service.reset();
  f.service.begin(app, global, false); f.service.finish(true);
  const serial = f.service.sessionSerial();
  for (let i = 0; i < 3; ++i) f.service.observeHarmonyScene(serial, 10, 0);
  assert.equal(f.service.currentHarmonyBlockReason(), '');
  f.service.observeHarmonyScene(serial, 10, 0);
  assert.match(f.service.currentHarmonyBlockReason(), /相机数据/);
  f.service.observeHarmonyScene(serial, 2, 1);
  assert.equal(f.service.currentHarmonyBlockReason(), '');
  for (let i = 0; i < 4; ++i) f.service.observeHarmonyScene(serial, 10, 1);
  assert.equal(f.service.currentHarmonyBlockReason(), '');
  for (let i = 0; i < 4; ++i) f.service.observeHarmonyScene(serial, 10, 0);
  f.service.applyLive({ ...harmony, frameGeneration: 'off' }, serial);
  assert.throws(() => f.service.applyLive(harmony, serial), /相机数据/);
  f.service.reset();
  assert.equal(f.service.currentHarmonyBlockReason(), '');
  console.log('Harmony GPU compatibility, saved intent, and session scene detection: PASS');
}
