const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), toasts = [], launches = [], stopped = [], replies = [];
let requests = [], state = 1;
let processInfo = { found: true, status: 'exited', exitCode: 1, exitCodeSource: 'waitpid' };
const singleton = value => ({ getInstance: () => value });
const helperDigests = new Map();
const deps = {
  'libsmoke_napi.so': { default: {
    startLinuxSteamLaunchBroker: () => true,
    drainLinuxSteamLaunchBroker: () => { const value = requests; requests = []; return value; },
    pollLinuxSteamLaunchSession: () => state,
    replyLinuxSteamLaunchBroker: (...args) => { replies.push(args); return true; }
  } },
  'libentry.so': { default: {
    getProcessList: () => [], queryWineProcess: () => processInfo, stopWineSession: id => stopped.push(id)
  } },
  '@kit.CoreFileKit': {}, '@kit.ArkUI': { promptAction: { showToast: value => toasts.push(value.message) } },
  '../model/AppModels': { DisplayMode: { DESKTOP: 1 }, WinePresentationMode: { MANAGED_WINDOWS: 1 } },
  './AppSessionService': { AppSessionService: singleton({ registerExternalLaunch() {} }) },
  './HostWindowTasks': { HostWindowTasks: singleton({ registerSteamGame(id) { launches.push(['registerGame', id]); } }) },
  './WineEngineService': { WineEngineService: singleton({
    launchExecutable: (...args) => { launches.push(args); return { pid: 100, sessionId: 'wine-100-1', reused: false }; },
    getMode: () => 1, getPresentationMode: () => 1
  }) },
  './ManagedSmokePayloadService': {
    readSmokeText: name => files.get(name) || '',
    writeSmokeText: (name, text) => files.set(name, text)
  },
  './ConfigurationFiles': { ConfigurationFiles: { fileHash: async name => helperDigests.get(name) || 'changed' } }
};
const output = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamGameLaunch.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: output, setInterval: () => 1, clearInterval() {}, require(id) {
  assert.ok(id in deps, id); return deps[id];
} });
const request = () => ({ id: 1, appId: '695630', role: 'game', cwd: '/z/SteamLinuxARM64/library/steamapps/common/NewPAL',
  argv: ['/z/SteamLinuxARM64/library/steamapps/common/NewPAL/NewPAL_Release.exe'], environment: ['SteamAppId=695630'] });
const service = new output.LinuxSteamGameLaunch();
async function main() {
service.start('/z/SteamLinuxARM64/library', '/results');
requests = [request()]; await service.tick();
assert.deepEqual(Array.from(launches[0][3]), ['SteamAppId=695630']);
state = 0; await service.tick();
const evidence = () => JSON.parse(files.get('/results/game-launch-695630.json'));
assert.equal(evidence().exitCode, 1); assert.equal(evidence().exitCodeSource, 'waitpid');
assert.equal(evidence().steamApiAuthenticationVerified, false);
assert.ok(toasts.some(text => text.includes('1')));
const toastCount = toasts.length;
files.set('/data/storage/el2/base/files/steamohos-native/game-launch-diagnostics.conf', 'wineLoader=1\n');
state = 1; requests = [request()]; await service.tick();
assert.ok(launches.filter(call => call[0] !== 'registerGame')[1][3].some(line => line.startsWith('WINEHUA_WINEDEBUG=')));
state = 0; processInfo = { found: true, status: 'exited', exitCode: 0, exitCodeSource: 'waitpid' }; await service.tick();
assert.equal(evidence().exitCode, 0); assert.equal(toasts.length, toastCount);
state = 1; requests = [request()]; await service.tick();
state = -1; processInfo = { found: false }; await service.tick();
assert.equal(evidence().status, 'wine_process_state_unavailable');
assert.equal(evidence().exitCode, null);
assert.equal(evidence().steamApiAuthenticationVerified, false);
const previousGame = JSON.stringify(evidence());
const lock = JSON.parse(fs.readFileSync(path.join(__dirname, '../entry/src/main/cpp/linux_steam/client_helpers.lock.json'), 'utf8'));
const gameRegistrations = launches.filter(call => call[0] === 'registerGame').length;
for (const [relative, record] of Object.entries(lock.files)) {
  const helper = { ...request(), role: record.role, argv: ['/z/SteamLinuxARM64/client/.steam/steam/' + relative],
    cwd: '/z/SteamLinuxARM64/client/.steam/steam' };
  state = 1; helperDigests.set(helper.argv[0], record.sha256);
  requests = [helper]; await service.tick();
  const helperToastCount = toasts.length;
  state = 0; processInfo = { found: true, status: 'exited', exitCode: 7, exitCodeSource: 'waitpid' }; await service.tick();
  assert.equal(toasts.length, helperToastCount, 'Auxiliary failures must not produce game-exit prompts');
  const helperEvidence = JSON.parse(files.get(`/results/client-helper-${record.role}-695630.json`));
  assert.equal(helperEvidence.exitCode, 7);
  assert.equal(helperEvidence.role, record.role);
  assert.equal(JSON.stringify(evidence()), previousGame, 'Helper completion cannot overwrite game evidence');
  assert.equal(launches.filter(call => call[0] === 'registerGame').length, gameRegistrations);
  const launchesBefore = launches.length;
  helperDigests.set(helper.argv[0], 'modified'); requests = [helper]; await service.tick();
  assert.equal(launches.length, launchesBefore, 'Changed client helper must never reach Wine');
}
const query = { ...request(), role: 'driver_query', cwd: '/z/SteamLinuxARM64/client/.steam/steam',
  argv: ['/z/SteamLinuxARM64/client/.steam/steam/bin/d3ddriverquery64.exe'] };
helperDigests.set(query.argv[0], lock.files['bin/d3ddriverquery64.exe'].sha256);
const launchCount = () => launches.filter(call => call[0] !== 'registerGame').length;
let before = launchCount();
state = 1; requests = [{ ...query, id: 10 }, { ...query, id: 11 }]; await service.tick();
assert.equal(launchCount(), before + 1);
assert.equal(service.pending.length, 1, 'Concurrent helpers must queue');
state = 0; await service.tick();
assert.equal(launchCount(), before + 2, 'Second helper starts when first finishes');
assert.equal(service.pending.length, 0);
state = 1; requests = [{ ...query, id: 12 }, { ...query, id: 12, cancel: true }]; await service.tick();
assert.equal(service.pending.length, 0, 'Cancelled queued work cannot start later');
before = launchCount(); service.stop(); state = 0; await service.tick();
assert.equal(launchCount(), before, 'Stopped service cannot start queued helpers');
console.log('Linux Steam Wine handoff, opt-in diagnostics, real exit status and unknown-state evidence: PASS');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
