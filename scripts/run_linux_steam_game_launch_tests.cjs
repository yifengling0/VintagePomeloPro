const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), toasts = [], launches = [];
let requests = [], state = 1;
let processInfo = { found: true, status: 'exited', exitCode: 1, exitCodeSource: 'waitpid' };
const singleton = value => ({ getInstance: () => value });
const deps = {
  'libsmoke_napi.so': { default: {
    startLinuxSteamLaunchBroker: () => true,
    drainLinuxSteamLaunchBroker: () => { const value = requests; requests = []; return value; },
    pollLinuxSteamLaunchSession: () => state,
    replyLinuxSteamLaunchBroker: () => true
  } },
  'libentry.so': { default: {
    getProcessList: () => [], queryWineProcess: () => processInfo
  } },
  '@kit.CoreFileKit': {}, '@kit.ArkUI': { promptAction: { showToast: value => toasts.push(value.message) } },
  '../model/AppModels': { DisplayMode: { DESKTOP: 1 }, WinePresentationMode: { MANAGED_WINDOWS: 1 } },
  './AppSessionService': { AppSessionService: singleton({ registerExternalLaunch() {} }) },
  './HostWindowTasks': { HostWindowTasks: singleton({ registerSteamGame() {} }) },
  './WineEngineService': { WineEngineService: singleton({
    launchExecutable: (...args) => { launches.push(args); return { pid: 100, sessionId: 'wine-100-1', reused: false }; },
    getMode: () => 1, getPresentationMode: () => 1
  }) },
  './ManagedSmokePayloadService': {
    readSmokeText: name => files.get(name) || '',
    writeSmokeText: (name, text) => files.set(name, text)
  }
};
const output = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamGameLaunch.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: output, setInterval: () => 1, clearInterval() {}, require(id) {
  assert.ok(id in deps, id); return deps[id];
} });
const request = () => ({ id: 1, appId: '695630', cwd: '/z/SteamLinuxARM64/library/steamapps/common/NewPAL',
  argv: ['/z/SteamLinuxARM64/library/steamapps/common/NewPAL/NewPAL_Release.exe'], environment: ['SteamAppId=695630'] });
const service = new output.LinuxSteamGameLaunch();
service.start('/z/SteamLinuxARM64/library', '/results');
requests = [request()]; service.tick();
assert.deepEqual(Array.from(launches[0][3]), ['SteamAppId=695630']);
state = 0; service.tick();
const evidence = () => JSON.parse(files.get('/results/game-launch-695630.json'));
assert.equal(evidence().exitCode, 1); assert.equal(evidence().exitCodeSource, 'waitpid');
assert.equal(evidence().steamApiAuthenticationVerified, false);
assert.ok(toasts.some(text => text.includes('1')));
const toastCount = toasts.length;
files.set('/data/storage/el2/base/files/steamohos-native/game-launch-diagnostics.conf', 'wineLoader=1\n');
state = 1; requests = [request()]; service.tick();
assert.ok(launches[1][3].some(line => line.startsWith('WINEHUA_WINEDEBUG=')));
state = 0; processInfo = { found: true, status: 'exited', exitCode: 0, exitCodeSource: 'waitpid' }; service.tick();
assert.equal(evidence().exitCode, 0); assert.equal(toasts.length, toastCount);
state = 1; requests = [request()]; service.tick();
state = -1; processInfo = { found: false }; service.tick();
assert.equal(evidence().status, 'wine_process_state_unavailable');
assert.equal(evidence().exitCode, null);
assert.equal(evidence().steamApiAuthenticationVerified, false);
console.log('Linux Steam Wine handoff, opt-in diagnostics, real exit status and unknown-state evidence: PASS');
