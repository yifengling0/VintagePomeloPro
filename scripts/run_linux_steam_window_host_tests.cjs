const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), writes = [], calls = [];
const base = '/data/storage/el2/base/files/steamohos-native/client-tmp';
const statePath = '/results/wine-window-state.json';
let focused = 5;
const tasks = [
  { key: 'wine-generation-1', toplevelId: 5, clientPid: 100 },
  { key: 'native-generation-1', source: 'steam-linux', toplevelId: 4, clientPid: 200 }
];
const singleton = value => ({ getInstance: () => value });
const deps = {
  '@kit.CoreFileKit': {},
  '../model/AppModels': {},
  './AppSessionService': {}, './WineWindowManager': {}, './WineEngineService': {},
  'libentry.so': { default: {
    getWineSession: () => ({ toplevelId: 5 }), getKeyboardFocusedToplevel: () => focused,
    bindExternalToplevel: (...args) => { calls.push(['bind', ...args]); return true; },
    notifyToplevelResize: (...args) => calls.push(['resize', ...args]),
    activateToplevel: id => calls.push(['activate', id]),
    unbindExternalToplevel: id => calls.push(['unbind', id]),
    stopWineSession: id => calls.push(['stop', id]),
    sendToplevelClose: id => calls.push(['close', id])
    ,moveExternalHost: (...args) => calls.push(['move', ...args])
  } },
  'libsmoke_napi.so': { default: { isLinuxSteamProcess: pid => pid === 200 } },
  './HostWindowTasks': { HostWindowTasks: singleton({ snapshot: () => tasks }) },
  './AppSettingsStore': { AppSettingsStore: singleton({ getGlobalSettings: () => ({ controllerOutputMode: 'dinput' }) }) },
  './GamepadManager': { GamepadManager: singleton({
    releaseAll: () => calls.push(['release']), setHostNavigation: enabled => calls.push(['navigation', enabled]),
    setOutputMode: mode => calls.push(['output', mode]), setButtonMappings: () => {}
  }) },
  './InputProfileService': { InputProfileService: singleton({ getProfile: id => ({ id, gamepadMappings: [] }) }) },
  './InputOverlayWindowCoordinator': { InputOverlayWindowCoordinator: singleton({
    getSnapshot: () => ({ session: {}, controlsVisible: true, opacity: 72 }), update: () => {}
  }) },
  './ManagedSmokePayloadService': {
    readSmokeText: name => files.get(name) || '',
    writeSmokeText: (name, text) => { files.set(name, text); writes.push([name, text]); }
  }
};
const output = {};
const code = ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamWindowHost.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;
vm.runInNewContext(code, { exports: output, clearInterval() {}, require(id) {
  assert.ok(id in deps, id); return deps[id];
} });
function fixture() {
  const host = new output.LinuxSteamWindowHost();
  Object.assign(host, { sessionId: 'wine-1', statePath, evidence: '/results/binding.json', started: Date.now() });
  return host;
}
function wine(minimized = 0, activation = 1) {
  files.set(statePath, JSON.stringify({ version: 1, x: 0, y: 0, width: 1024, height: 640,
    minimized, activation, closed: 0 }));
}
function native(mode = 0, minimized = 0, closed = 0, moveRequest = 0) {
  files.set(base + '/wine-window-native.json', JSON.stringify({ version: 1, window: 123,
    mode, minimized, activation: 1, closed, moveRequest }));
}
const host = fixture();
wine(); native(); host.tick();
assert.deepEqual(calls.find(c => c[0] === 'bind'), ['bind', 4, 5, 200, 100, 0, 0, 1024, 640]);
assert.ok(calls.some(c => c[0] === 'output' && c[1] === 'dinput'));
assert.ok(!calls.some(c => c[0] === 'navigation' && c[1]), 'Steam SDL consumes the hub without duplicate keyboard navigation');
const count = writes.length; host.tick();
assert.equal(writes.length, count, 'Unchanged state cannot cause control/activation loops');
native(0,0,0,1); host.tick(); host.tick();
assert.deepEqual(calls.filter(c => c[0] === 'move'), [['move', 4, 5]], 'One native move request targets the bound Wine host once');
native(2); host.tick();
assert.match(files.get(statePath + '.control'), /^\d+ 2 0 1\n$/);
native(2, 1); host.tick(); wine(1); host.tick();
wine(0, 2); host.tick();
assert.match(files.get(base + '/wine-window-command.txt'), /^\d+ 1\n$/);
const restoreWrites = writes.length; host.tick();
assert.equal(writes.length, restoreWrites, 'Stale native minimized state cannot re-minimize Wine after taskbar restore');
native(2, 0); host.tick();
focused = 99; host.tick();
assert.ok(calls.some(c => c[0] === 'output' && c[1] === 'dinput'), 'Game focus restores the original controller mode');
const beforeClose = calls.length; native(0, 0, 1); host.tick();
assert.deepEqual(calls.slice(beforeClose), [['unbind', 5], ['stop', 'wine-1']]);
assert.equal(host.sessionId, '', 'Closed native client cannot rebind a stopped Wine host');
native(); wine(); const replaced = fixture(); replaced.tick();
tasks[1].key = 'native-generation-2'; replaced.tick();
assert.equal(replaced.sessionId, '', 'A reused Wayland ID cannot bind a different client generation');
console.log('Wine/native window state, no activation loops, taskbar restore, focus profile, exit and generation: PASS');
