const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), writes = [], calls = [];
const base = '/data/storage/el2/base/files/steamohos-native/client-tmp';
const statePath = '/results/wine-window-state.json';
let focused = 5;
let launcherForeground = false;
const tasks = [
  { key: 'wine-generation-1', toplevelId: 5, clientPid: 100 },
  { key: 'native-generation-1', source: 'steam-linux', toplevelId: 4, clientPid: 200 }
];
const preferencesOutput = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/model/LinuxSteamWindowPreferences.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: preferencesOutput });
const singleton = value => ({ getInstance: () => value });
const deps = {
  '@kit.CoreFileKit': {},
  '../model/AppModels': {DisplayMode:{DESKTOP:'desktop'},WinePresentationMode:{DESKTOP:'desktop'}},
  './AppSessionService': {}, './WineWindowManager': {}, './WineEngineService': {},
  './ConfigurationFiles': {},
  '../model/LinuxSteamWindowPreferences': preferencesOutput,
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
  './HostWindowTasks': { HostWindowTasks: singleton({ snapshot: () => tasks, isLauncherForeground: () => launcherForeground }) },
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
vm.runInNewContext(code, { exports: output, clearInterval() {}, setInterval: () => 1, setTimeout, require(id) {
  assert.ok(id in deps, id); return deps[id];
} });
function fixture() {
  const host = new output.LinuxSteamWindowHost();
  Object.assign(host, { sessionId: 'wine-1', statePath, evidence: '/results/binding.json', started: Date.now() });
  return host;
}
function wine(minimized = 0, activation = 1, mode = 0, controlSerial = 100000, width = 1024, height = 640) {
  files.set(statePath, JSON.stringify({ version: 1, x: 0, y: 0, width, height,
    minimized, activation, closed: 0, mode, controlSerial }));
}
function native(mode = 0, minimized = 0, closed = 0, moveRequest = 0, normalWidth, normalHeight) {
  files.set(base + '/wine-window-native.json', JSON.stringify({ version: 1, window: 123,
    mode, minimized, activation: 1, closed, moveRequest, normalWidth, normalHeight }));
}
const host = fixture();
wine(); native(); host.tick();
assert.deepEqual(calls.find(c => c[0] === 'bind'), ['bind', 4, 5, 200, 100, 0, 0, 1024, 640]);
assert.deepEqual(calls.find(c => c[0] === 'resize'), ['resize', 4, 1024, 640]);
assert.ok(calls.some(c => c[0] === 'output' && c[1] === 'dinput'));
assert.ok(!calls.some(c => c[0] === 'navigation' && c[1]), 'Steam SDL consumes the hub without duplicate keyboard navigation');
launcherForeground = true;
const activations = calls.filter(c => c[0] === 'activate').length;
wine(0, 2); host.tick();
assert.equal(calls.filter(c => c[0] === 'activate').length, activations, 'Steam activation cannot steal focus from Deck');
launcherForeground = false; wine(); host.tick();
const count = writes.length; host.tick();
assert.equal(writes.length, count, 'Unchanged state cannot cause control/activation loops');
native(0,0,0,1); host.tick(); host.tick();
assert.deepEqual(calls.filter(c => c[0] === 'move'), [['move', 4, 5]], 'One native move request targets the bound Wine host once');
const resizedBefore = calls.filter(c => c[0] === 'resize').length;
native(2); wine(0,1,0,100000,1200,800); host.tick();
assert.equal(calls.filter(c => c[0] === 'resize').length, resizedBefore,
  'Do not resize the X11 root from Wine geometry before presentation acknowledgment');
assert.match(files.get(statePath + '.control'), /^\d+ 2 0 1\n$/);
wine(0,1,2,100000,1200,800); host.tick();
native(2, 1); host.tick(); wine(1,1,2); host.tick();
wine(0, 2,2); host.tick();
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
// Withdrawing Big Picture must not briefly restore the desktop rectangle.
const transition = fixture(); native(2); wine(0,1,2); transition.tick();
const beforeWithdrawal = writes.length;
files.set(base + '/wine-window-native.json', JSON.stringify({version:1,window:0,mode:0,minimized:0,activation:3,closed:0}));
transition.tick();
assert.equal(writes.length, beforeWithdrawal, 'No window means no new presentation command');
const sized = fixture(); native(1,0,0,0,1024,640); wine(0,1,1); sized.tick();
native(0,0,0,0,960,600); wine(0,1,1); sized.tick();
assert.match(files.get(statePath+'.control'), /^\d+ 0 0 1 960 600\n$/,
  'Window restoration carries the native normal size instead of the stale maximized Wine rectangle');
native(0,0,0,0,4096,4096); sized.tick();
assert.match(files.get(statePath+'.control'), /^\d+ 0 0 1\n$/,'invalid dimensions cannot resize the host');
console.log('Wine/native window state, no activation loops, taskbar restore, focus profile, exit and generation: PASS');

// A delayed Wine configure must not resize Xwayland before it has settled.
const pendingGeometry = fixture(); native(1); wine(0,1,1);
files.set(statePath, JSON.stringify({version:1,x:0,y:0,width:700,height:440,
  mode:1,controlSerial:100000,minimized:0,activation:1,closed:0,geometryReady:0}));
const beforePending = calls.filter(c => c[0] === 'resize').length;
pendingGeometry.tick();
assert.equal(calls.filter(c => c[0] === 'resize').length,beforePending);

(async () => {
  const initialPath = '/drive/hmosdeck/session/wine-window-state.json';
  files.set(initialPath,'old window state');
  files.set(initialPath+'.control','999 1 0 1 700 440\n');
  deps['@kit.CoreFileKit'].fileIo = {
    accessSync: name => files.has(name), unlinkSync: name => files.delete(name),
    OpenMode:{CREATE:1,WRITE_ONLY:2,TRUNC:4},openSync:()=>({fd:1}),
    writeSync(){},closeSync(){}
  };
  deps['./ManagedSmokePayloadService'].ensureSmokeDirectory = () => true;
  deps['./WineWindowManager'].WineWindowManager = singleton({getDesktopMode:()=>true});
  deps['./WineEngineService'].WineEngineService = singleton({launchExecutable() {
    assert.ok(!files.has(initialPath));
    assert.ok(!files.has(initialPath+'.control'),'Old HWND presentation serial is cleared before launch');
    files.set(initialPath,JSON.stringify({version:1,width:700,height:440,closed:0,geometryReady:0}));
    setTimeout(()=>files.set(initialPath,JSON.stringify({version:1,width:960,height:600,closed:0,geometryReady:1})),30);
    return {pid:100,sessionId:'new-host',reused:false};
  }});
  deps['./AppSessionService'].AppSessionService = singleton({registerExternalLaunch(){}});
  const payload = new Uint8Array(1024);payload[0]=77;payload[1]=90;
  const started = new output.LinuxSteamWindowHost();
  await started.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session');
  assert.equal(files.get(base+'/wine-window-geometry.txt'),'960x600\n',
    'Initial Xwayland size comes from settled geometry, never the intermediate 700x440 publication');
  console.log('Fresh HWND command lifetime, delayed initial configure and settled resize handshake: PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
