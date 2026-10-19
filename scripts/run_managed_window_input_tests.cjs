const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[3]));
const models = require(path.resolve(process.argv[2], 'model/AppModels.js'));
function load(name, dependencies = {}) {
  const result = {};
  const file = path.resolve(__dirname, '../entry/src/main/ets/service/' + name + '.ets');
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText, { exports: result, require(id) { assert.ok(id in dependencies, id); return dependencies[id]; } });
  return result;
}
const taskModule = load('HostWindowTasks');
const tasks = taskModule.HostWindowTasks.getInstance();
tasks.onEvent(10, 'created', JSON.stringify({ clientPid: 100 }), true);
tasks.registerSteamGame('game');
tasks.onEvent(11, 'created', JSON.stringify({ sessionId: 'game' }));
const settings = { controllerOutputMode: 'dinput', desktopInputProfileId: 'default',
  showVirtualControls: true, controlOpacity: 72, virtualInputEnabled: true };
const profiles = models.createBuiltInInputProfiles();
const steamProfile = profiles.find(p => p.id === 'builtin-steam-navigation');
assert.ok(steamProfile);
assert.deepEqual(Array.from(steamProfile.elements[0].bindings, b => b.code), [103, 106, 108, 105]);
assert.equal(steamProfile.gamepadMappings.find(m => m.buttonCode === 2301).evdevCode, 28);
assert.equal(steamProfile.gamepadMappings.find(m => m.buttonCode === 2302).evdevCode, 1);
const native = { controllerSetOutputMode() {}, controllerResetSource() {} };
const sent = [];
const gamepadModule = load('GamepadManager', {
  '@kit.SensorServiceKit': { vibrator: {} }, '@kit.BasicServicesKit': {},
  './InputRouter': { KeyMouseGamepadSink: class {} }, '../model/AppModels': models,
  './LogService': { hilog: { info() {}, warn() {}, error() {} } }, 'libentry.so': { default: native }
});
const manager = gamepadModule.GamepadManager.getInstance();
manager.setSink({ onButton(key, pressed) { sent.push([key, pressed]); }, onPointerDelta() {} });
manager.setOutputMode('keyboard_legacy'); manager.setHostNavigation(true);
manager.handleAxis(0, -0.9, 0);
assert.ok(sent.some(([key, down]) => key === 105 && down), 'Steam left stick navigates with arrows');
manager.setHostNavigation(false); manager.handleAxis(0, -0.9, 0);
assert.ok(sent.some(([key, down]) => key === 30 && down), 'Ordinary legacy game keeps WASD');
manager.setButtonMappings(steamProfile.gamepadMappings); manager.handleKey(2301, true); manager.handleKey(2301, false);
assert.ok(sent.some(([key, down]) => key === 28 && down));
manager.releaseAll();
let inputSession;
const attached = [], hidden = [], focused = [];
const profilesService = { initialize: async () => {}, getProfile: id =>
  profiles.find(p => p.id === id) || models.createDefaultInputProfile() };
const coordinator = { attach: async (...args) => { attached.push(args); },
  setVisible: async visible => { hidden.push(visible); }, syncBounds: async () => {} };
const managed = load('ManagedWindowInput', {
  '@kit.AbilityKit': {}, '@kit.ArkUI': {},
  'libentry.so': { default: { activateToplevel: id => focused.push(id) } },
  '../model/AppModels': models,
  './AppSettingsStore': { AppSettingsStore: { getInstance: () => ({ getGlobalSettings: () => settings }) } },
  './GamepadManager': gamepadModule, './HostWindowTasks': taskModule,
  './InputDispatcher': { InputDispatcher: { getInstance: () => ({
    setSession: session => { inputSession = session; }, releaseAll() {}
  }) } },
  './InputOverlayWindowCoordinator': { InputOverlayWindowCoordinator: { getInstance: () => coordinator } },
  './InputProfileService': { InputProfileService: { getInstance: () => profilesService } }
}).ManagedWindowInput.getInstance();

(async () => {
  managed.foreground(10, {}, {}, {}); await managed.operations;
  assert.equal(manager.getOutputMode(), 'dinput');
  assert.equal(inputSession.toplevelId, 10);
  assert.equal(attached[0][5], 'builtin-steam-navigation');
  managed.foreground(11, {}, {}, {}); await managed.operations;
  assert.equal(manager.getOutputMode(), 'dinput');
  assert.equal(inputSession.sessionId, 'game');
  managed.background(10); await managed.operations;
  assert.equal(hidden.length, 0, 'Late background from Steam must not hide the game controls');
  managed.background(11); await managed.operations;
  assert.equal(hidden.at(-1), false);
  let release;
  profilesService.initialize = () => new Promise(resolve => { release = resolve; });
  managed.foreground(10, {}, {}, {});
  await Promise.resolve(); await Promise.resolve();
  managed.foreground(11, {}, {}, {});
  profilesService.initialize = async () => {};
  release(); await managed.operations;
  assert.equal(inputSession.toplevelId, 11);
  assert.equal(focused.at(-1), 11, 'Stale profile lookup cannot steal focus');
  console.log('Steam navigation profile, physical stick/A/B, foreground overlay ownership, game DInput restoration and stale async focus: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
