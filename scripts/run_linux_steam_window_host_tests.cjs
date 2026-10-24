const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), writes = [], calls = [];
const base = '/data/storage/el2/base/files/steamohos-native/client-tmp';
const statePath = '/results/wine-window-state.json';
let fakeNow = 100000, initialReadyDelay = 1000, readyAt = Infinity, nativeSessionState = 'running';
let onSleep = () => {};
let focused = 5;
let launcherForeground = false, steamGameForeground = false;
let nativeOwnershipAvailable = true;
let failDiagnosticWrites = false;
let externalResizing = false;
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
    getWineSession: () => ({ toplevelId: 5, state: nativeSessionState }), getKeyboardFocusedToplevel: () => focused,
    bindExternalToplevel: (...args) => { calls.push(['bind', ...args]); return true; },
    notifyToplevelResize: (...args) => calls.push(['resize', ...args]),
    activateToplevel: id => calls.push(['activate', id]),
    unbindExternalToplevel: id => calls.push(['unbind', id]),
    stopWineSession: id => calls.push(['stop', id]),
    sendToplevelClose: id => calls.push(['close', id])
    ,moveExternalHost: (...args) => { calls.push(['move', ...args]); return true; },
    isExternalHostResizing: () => externalResizing
  } },
  'libsmoke_napi.so': { default: { isLinuxSteamProcess: pid => nativeOwnershipAvailable && pid === 200 } },
  './HostWindowTasks': { HostWindowTasks: singleton({ snapshot: () => tasks,
    refreshLinuxSteamOwnership: owns => {for(const task of tasks)if(owns(task.clientPid))task.source='steam-linux';},
    activateSteamLaunchPrompt: async (...args) => {calls.push(['prompt', ...args]); return true;},
    isLauncherForeground: () => launcherForeground, isSteamGameForeground: () => steamGameForeground }) },
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
    readSmokeText: name => {
      if(name === '/drive/hmosdeck/session/wine-window-state.json' && fakeNow >= readyAt)
        files.set(name,JSON.stringify({version:1,width:960,height:600,closed:0,geometryReady:1}));
      return files.get(name) || '';
    },
    writeSmokeText: (name, text) => {
      if (failDiagnosticWrites && /\/binding\.json(?:\.candidates)?$/.test(name)) throw new Error('ENOSPC');
      files.set(name, text); writes.push([name, text]);
    }
  }
};
const output = {};
const code = ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamWindowHost.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;
vm.runInNewContext(code, { exports: output, clearInterval() {}, setInterval: () => 1, Date: {now: () => fakeNow},
  setTimeout(callback, delay) { fakeNow += delay; onSleep(); return setTimeout(callback,0); }, require(id) {
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
// A fast native child may announce the surface before its trusted ancestry
// is recorded. The live task must recover without a synthetic created event.
const pendingOwnership = fixture();
tasks[1].source = 'wine'; nativeOwnershipAvailable = false;
wine(); native(); pendingOwnership.tick();
assert.equal(calls.some(c => c[0] === 'bind'),false,'unverified surfaces cannot bind');
nativeOwnershipAvailable = true; pendingOwnership.tick();
assert.equal(tasks[1].source,'steam-linux');
assert.ok(calls.some(c => c[0] === 'bind'),'late broker registration recovers an existing native surface');
assert.equal(tasks[1].key,'native-generation-1','reclassification preserves the original window generation');
calls.length=0; writes.length=0;
const host = fixture();
wine(); native(); host.tick();
assert.deepEqual(calls.find(c => c[0] === 'bind'), ['bind', 4, 5, 200, 100, 0, 0, 1024, 640]);
assert.deepEqual(calls.find(c => c[0] === 'resize'), ['resize', 4, 1024, 640]);
assert.equal(host.isForeground(),true,'Wine host focus permits recovery');
focused = 4;
assert.equal(host.isForeground(),true,'bound Linux content focus also permits recovery without a click');
focused = 99;
assert.equal(host.isForeground(),false,'unrelated window focus cannot navigate Steam');
focused = 4; launcherForeground = true;
assert.equal(host.isForeground(),false,'Deck foreground blocks recovery even with stale content focus');
launcherForeground = false; steamGameForeground = true;
assert.equal(host.isForeground(),false,'a foreground game blocks stale Steam focus');
steamGameForeground = false;
for (const task of tasks) {
  task.minimized = true;
  assert.equal(host.isForeground(),false,'either minimized binding endpoint blocks recovery');
  task.minimized = false;
  const key = task.key; task.key += '-replaced';
  assert.equal(host.isForeground(),false,'reused endpoint IDs must not authorize recovery');
  task.key = key;
}
const binding = host.lastGeometry; host.lastGeometry = '';
assert.equal(host.isForeground(),false,'unbound content cannot authorize recovery');
host.lastGeometry = binding;
const session = host.sessionId; host.sessionId = '';
assert.equal(host.isForeground(),false,'stopped host cannot authorize recovery');
host.sessionId = session; focused = 5;

assert.ok(calls.some(c => c[0] === 'output' && c[1] === 'dinput'));
assert.ok(!calls.some(c => c[0] === 'navigation' && c[1]), 'Steam SDL consumes the hub without duplicate keyboard navigation');
launcherForeground = true;
const activations = calls.filter(c => c[0] === 'activate').length;
wine(0, 2); host.tick();
assert.equal(calls.filter(c => c[0] === 'activate').length, activations, 'Steam activation cannot steal focus from Deck');
launcherForeground = false; wine(); host.tick();
steamGameForeground = true; focused = 99;
native(1); wine(0,3); const beforeGameFocus = calls.filter(c => c[0] === 'activate').length; host.tick();
assert.match(files.get(statePath+'.control'), /^\d+ 1 0 0\n$/, 'Background Steam geometry does not request foreground');
assert.equal(calls.filter(c => c[0] === 'activate').length,beforeGameFocus,'Steam cannot reclaim game focus');
steamGameForeground = false; focused = 5; native(); wine(); host.tick();
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

// Native corner resize follows the applied HWND geometry until the X11
// supervisor acknowledges the final command. Stale normalWidth cannot undo it.
tasks[1].key='native-generation-1';
const resizing=fixture();native(0,0,0,0,960,600);wine(0,1,0,100000,960,600);resizing.tick();
function resizeNative(request,edge,normalWidth=960,normalHeight=600,commandSerial=0,mode=0,window=123) {
  files.set(base+'/wine-window-native.json',JSON.stringify({version:1,window,mode,minimized:0,
    activation:1,closed:0,moveRequest:0,resizeRequest:request,resizeEdges:edge,normalWidth,normalHeight,commandSerial}));
}
externalResizing=true;resizeNative(1,10);resizing.tick();
assert.deepEqual(calls.filter(c=>c[0]==='move').at(-1),['move',4,5,10]);
wine(0,1,0,100000,1100,700);resizing.tick();
assert.match(files.get(base+'/wine-window-command.txt'),/^\d+ 2 1100 700\n$/);
const controlBeforeResize=files.get(statePath+'.control');
resizing.tick();assert.equal(files.get(statePath+'.control'),controlBeforeResize);
externalResizing=false;resizing.tick();
assert.equal(files.get(statePath+'.control'),controlBeforeResize,'do not snap to stale pre-drag native size');
resizeNative(1,10,1100,700,resizing.resizeCommandSerial);resizing.tick();
assert.equal(resizing.resizeFollow,false);assert.match(files.get(statePath+'.control'),/ 1100 700\n$/);
const callsBeforeInvalid=calls.filter(c=>c[0]==='move').length;
resizeNative(2,3,1100,700);resizing.tick();
assert.equal(calls.filter(c=>c[0]==='move').length,callsBeforeInvalid);
// Explicit mode changes and replacement native windows end old drags.
externalResizing=true;resizeNative(3,5,1100,700);resizing.tick();assert.equal(resizing.resizeFollow,true);
resizeNative(3,5,1100,700,0,2);resizing.tick();assert.equal(resizing.resizeFollow,false);
externalResizing=false;resizing.stop();
assert.equal(resizing.resizeFollow,false);
console.log('Interactive resize: applied geometry, command acknowledgment, invalid edge, mode change and stop: PASS');

// Auxiliary dialogs request attention once per activation. Withdrawn/closed
// windows cannot replay stale requests; the registry checks the live Play intent.
const promptHost = fixture(); wine(); native(); promptHost.tick();
const promptCount = () => calls.filter(c => c[0] === 'prompt').length;
const beforePrompt = promptCount();
const promptNative = (activation, extra = {}) => files.set(base+'/wine-window-native.json',
  JSON.stringify({version:1,window:123,mode:0,minimized:0,activation,closed:0,activeChild:456,...extra}));
promptNative(10); promptHost.tick(); promptHost.tick();
assert.equal(promptCount(),beforePrompt+1,'a new auxiliary activation requests attention exactly once');
assert.deepEqual(calls.filter(c=>c[0]==='prompt').at(-1),['prompt','wine-generation-1','wine-1']);
promptNative(11,{window:0}); promptHost.tick();
assert.equal(promptCount(),beforePrompt+1,'withdrawal cannot focus a stale dialog');
promptNative(11,{activeChild:0}); promptHost.tick();
assert.equal(promptCount(),beforePrompt+1,'primary activation never requests dialog attention');
promptNative(11); promptHost.tick();
assert.equal(promptCount(),beforePrompt+2,'a later valid dialog activation is honored');
promptNative(12,{closed:1}); promptHost.tick();
assert.equal(promptCount(),beforePrompt+2,'closed clients cannot request attention');
console.log('Auxiliary activation deduplication, primary-only, withdrawal and close: PASS');


// A full/unavailable evidence directory must not prevent binding, resize or
// shutdown. Exercise both the success and timer-error diagnostic paths.
const diagnosticFailure = fixture(); native(1); wine(0,1,1);
failDiagnosticWrites = true;
const beforeDiagnosticFailure = calls.length;
assert.doesNotThrow(() => diagnosticFailure.tick());
assert.ok(calls.slice(beforeDiagnosticFailure).some(c => c[0] === 'bind'),
  'optional binding evidence cannot interrupt an established native binding');
assert.doesNotThrow(() => diagnosticFailure.record('binding_error:ENOSPC'),
  'the timer catch must not throw while recording another I/O failure');
const missingSurface = fixture(); tasks[1].source = 'wine'; nativeOwnershipAvailable = false;
assert.doesNotThrow(() => missingSurface.tick(), 'candidate diagnostics are optional too');
assert.equal(missingSurface.sessionId,'wine-1','an evidence error cannot stop the pending host');
nativeOwnershipAvailable = true;
assert.doesNotThrow(() => missingSurface.tick());
assert.ok(missingSurface.host > 0,'a later verified surface still binds after evidence write failure');
native(0,0,1);
assert.doesNotThrow(() => diagnosticFailure.tick());
assert.equal(diagnosticFailure.sessionId,'','native exit cleanup runs even with unavailable evidence');
failDiagnosticWrites = false;
console.log('Binding, pending surface recovery and exit tolerate diagnostic ENOSPC: PASS');

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
    readyAt = fakeNow + initialReadyDelay;
    return {pid:100,sessionId:'new-host',reused:false};
  }});
  deps['./AppSessionService'].AppSessionService = singleton({registerExternalLaunch(){}});
  const payload = new Uint8Array(1024);payload[0]=77;payload[1]=90;
  const started = new output.LinuxSteamWindowHost();
  await started.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session');
  assert.equal(files.get(base+'/wine-window-geometry.txt'),'960x600\n',
    'Initial Xwayland size comes from settled geometry, never the intermediate 700x440 publication');
  started.stop();
  initialReadyDelay = 1000;
  files.set(base+'/wine-window-native.json','live native mode');
  files.set(base+'/wine-window-command.txt','57 1\n');
  const replacement = new output.LinuxSteamWindowHost();
  await replacement.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session',true);
  assert.equal(replacement.controlSerial,57,'replacement commands continue the live native sequence');
  assert.equal(files.get(base+'/wine-window-native.json'),'live native mode','replacement retains native presentation');
  assert.equal(replacement.hasExited(),false);
  nativeSessionState='stopped';
  assert.equal(replacement.hasExited(),true,'confirmed Wine worker exit is observable even with stale HWND metadata');
  assert.equal(await replacement.restore(),false,'dead HWND cannot be activated');
  nativeSessionState='running'; replacement.stop();
  initialReadyDelay = 12000;
  const cold = new output.LinuxSteamWindowHost();
  const coldAt = fakeNow;
  await cold.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session');
  assert.ok(fakeNow - coldAt >= 12000,'cold worker can report settled geometry after the former five-second cutoff');
  assert.equal(cold.sessionId,'new-host'); cold.stop();
  initialReadyDelay = 30000;
  const firstUse = new output.LinuxSteamWindowHost();
  const firstUseAt = fakeNow;
  await firstUse.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session');
  assert.ok(fakeNow - firstUseAt >= 30000,'a live first-use worker survives the former twenty-second cutoff');
  assert.equal(firstUse.sessionId,'new-host'); firstUse.stop();
  initialReadyDelay = 130000;
  const timeoutHost = new output.LinuxSteamWindowHost();
  await assert.rejects(timeoutHost.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session'), /120 秒/);
  assert.equal(timeoutHost.sessionId,'','bounded timeout stops its pending Wine worker');
  const exited = new output.LinuxSteamWindowHost(); nativeSessionState = 'stopped';
  const exitedAt = fakeNow;
  await assert.rejects(exited.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session'), /进程已退出/);
  assert.equal(fakeNow,exitedAt,'known process exit fails without waiting for the startup timeout');
  nativeSessionState = 'running';
  const cancelled = new output.LinuxSteamWindowHost();
  onSleep = () => { cancelled.stop(); onSleep = () => {}; };
  await assert.rejects(cancelled.start({resourceManager:{getRawFileContent:async()=>payload}},'/drive','/drive/hmosdeck/session'), /已取消/);
  assert.ok(!files.has(base+'/wine-window-geometry.txt'),'cancelled startup cannot publish late geometry');
  console.log('Fresh HWND lifetime, cold worker, bounded timeout, early exit, cancellation and settled geometry: PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
