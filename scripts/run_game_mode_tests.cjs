const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(process.argv[2]);
const ts = require(path.resolve(process.argv[3]));
const models = require(path.join(root, 'model/AppModels.js'));
const rules = require(path.join(root, 'model/GameModeSettings.js'));
assert.deepEqual(rules.normalizeGameModeSettings({ scaling: 'bad', frameLimit: 120 }), { scaling: 'linear', frameLimit: 0, sharpness: 10 });
assert.deepEqual(rules.normalizeGameModeSettings({ scaling: 'fsr', frameLimit: 60, sharpness: 0 }), { scaling: 'fsr', frameLimit: 60, sharpness: 0 });
assert.equal(rules.normalizeGameModeSettings({ scaling: 'fsr', sharpness: NaN }).sharpness, 10);
assert.equal(rules.normalizeGameModeSettings({ scaling: 'fsr', sharpness: 21 }).sharpness, 10);
assert.equal(rules.sameGamePresentation({ enabled: true, settings: { scaling: 'fsr', frameLimit: 30, sharpness: 0 } },
  { enabled: true, settings: { scaling: 'fsr', frameLimit: 30, sharpness: 20 } }), false);
assert.equal(rules.resolveGamePresentation(true, 'off').enabled, false);
assert.equal(rules.resolveGamePresentation(false, 'on').enabled, true);
assert.equal(rules.resolveGamePresentation(true, undefined).enabled, true);
assert.equal(rules.resolveGamePresentation(false, undefined).enabled, false);
assert.deepEqual(rules.resolveGamePresentation(true, 'follow', { scaling: 'linear', frameLimit: 60 },
  { scaling: 'integer', frameLimit: 30 }).settings, { scaling: 'integer', frameLimit: 30, sharpness: 10 });
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
    './AppSettingsStore': { AppSettingsStore: { getInstance: () => ({ getAppSettings: () => appSettings }) } }
  };
  vm.runInNewContext(code, { exports, require: name => {
    assert.ok(Object.hasOwn(dependencies, name), name); return dependencies[name];
  }, AppStorage: { get: key => values.get(key), setOrCreate: (key, value) => values.set(key, value) } });
  return { service: exports.GameModeService, values, calls, targets, appSettings, dependencies };
}
const app = { id: 'game', launchTarget: { kind: models.LaunchKind.WINE_EXE } };
const global = models.createDefaultGlobalSettings();
global.gameMode = { scaling: 'nearest', frameLimit: 30 };
{
  const f = fixture(); f.values.set('vpp.game.browsing', true);
  assert.equal(f.calls.length, 0); // entering library does not start a presenter
  f.service.begin(app, global, false);
  assert.deepEqual(f.calls[0], [true, 'nearest', 30, 10]);
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
  assert.deepEqual(f.service.currentSettings(), f.appSettings.gameModeSettings, 'Panel reads the active per-game policy');
  const serial = f.service.sessionSerial();
  f.service.applyLive({ scaling: 'integer', frameLimit: 30 }, serial);
  assert.deepEqual(f.calls.at(-1), [true, 'integer', 30, 10]);
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
