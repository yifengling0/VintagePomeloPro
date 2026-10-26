const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), toasts = [], launches = [], stopped = [], replies = [];
let requests = [], state = 1, replyAccepted = true, failAudit = false, failRecord = false, failLaunch = false;
let processInfo = { found: true, status: 'exited', exitCode: 1, exitCodeSource: 'waitpid' };
const singleton = value => ({ getInstance: () => value });
const helperDigests = new Map();
const completions = [];
let runtimeLeases=0;
const deps = {
  './LinuxProtonRuntimeService': {LinuxProtonRuntimeService:singleton({holdGame:()=>{runtimeLeases++;return()=>runtimeLeases--;}})},
  'libsmoke_napi.so': { default: {
    startLinuxSteamLaunchBroker: () => true,
    drainLinuxSteamLaunchBroker: () => { const value = requests; requests = []; return value; },
    pollLinuxSteamLaunchSession: () => state,
    replyLinuxSteamLaunchBroker: (...args) => { replies.push(args); return replyAccepted; }
  } },
  'libentry.so': { default: {
    getProcessList: () => [], queryWineProcess: () => processInfo, stopWineSession: id => stopped.push(id)
  } },
  '@kit.CoreFileKit': { fileIo: { listFileSync() { if (failAudit) throw Error('library updating'); return []; }, statSync() { throw Error('file removed'); } } }, '@kit.ArkUI': { promptAction: { showToast: value => toasts.push(value.message) } },
  '../model/AppModels': { DisplayMode: { DESKTOP: 1 }, WinePresentationMode: { MANAGED_WINDOWS: 1 } },
  './AppSessionService': { AppSessionService: singleton({ registerExternalLaunch() {} }) },
  './HostWindowTasks': { HostWindowTasks: singleton({ registerSteamGame(id) { launches.push(['registerGame', id]); }, finishSteamGame(id, known) { completions.push([id, known]); } }) },
  './AppSettingsStore': { AppSettingsStore: singleton({ getGlobalSettings: () => ({ wineLanguage: 'zh_CN' }) }) },
  './WineEngineService': { WineEngineService: singleton({
    launchExecutable: (...args) => { if (failLaunch) throw Error('launch failed'); launches.push(args); return { pid: 100, sessionId: 'wine-100-1', reused: false }; },
    getMode: () => 1, getPresentationMode: () => 1
  }) },
  './ManagedSmokePayloadService': {
    readSmokeText: name => files.get(name) || '',
    writeSmokeText: (name, text) => { if (failRecord) throw Error('diagnostic volume unavailable'); files.set(name, text); }
  },
  './ConfigurationFiles': { ConfigurationFiles: { fileHash: async name => helperDigests.get(name) || 'changed' } }
};
const paths = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamPaths.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: paths });
deps['./LinuxSteamPaths'] = paths;
const output = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamGameLaunch.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: output, setImmediate, setInterval: () => 1, clearInterval() {}, require(id) {
  assert.ok(id in deps, id); return deps[id];
} });
const request = () => ({ id: 1, appId: '695630', role: 'game', cwd: '/z/SteamLinuxARM64/library/steamapps/common/NewPAL',
  argv: ['/z/SteamLinuxARM64/library/steamapps/common/NewPAL/NewPAL_Release.exe'], environment: ['SteamAppId=695630'] });
const service = new output.LinuxSteamGameLaunch();
async function main() {
service.start('/z/SteamLinuxARM64/library', '/results', '/z');
requests = [request()]; await service.tick();
assert.equal(launches[0][0], 'Z:\\SteamLinuxARM64\\library\\steamapps\\common\\NewPAL\\NewPAL_Release.exe');
assert.deepEqual(Array.from(launches[0][3]), ['SteamAppId=695630']);
state = 0; await service.tick();
const evidence = () => JSON.parse(files.get('/results/game-launch-695630.json'));
assert.equal(files.get('/results/last-game-launch.json'), files.get('/results/game-launch-695630.json'));
assert.ok(Number.isSafeInteger(evidence().updatedAt) && evidence().updatedAt > 0);
assert.deepEqual(completions.at(-1), ['wine-100-1', true]);
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
assert.deepEqual(completions.at(-1), ['wine-100-1', false], 'Unknown process state cannot return focus to Steam');
assert.equal(evidence().status, 'wine_process_state_unavailable');
assert.equal(evidence().exitCode, null);
assert.equal(evidence().steamApiAuthenticationVerified, false);
state = 1; requests = [request()]; await service.tick();
state = 0; processInfo = { found: true, status: 'exited', exitCode: null, exitCodeSource: 'ncp-exit' }; await service.tick();
assert.equal(evidence().status,'wine_process_ended_exit_code_unavailable');
assert.equal(evidence().exitCode,null,'NCP death is not a fabricated successful exit code');
assert.deepEqual(completions.at(-1),['wine-100-1',true],'Authoritative NCP death can end focus ownership');
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
  assert.equal(files.get('/results/last-client-helper.json'), JSON.stringify(helperEvidence));
  assert.equal(files.get('/results/last-game-launch.json'), previousGame);
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
// Diagnostics cannot become lifecycle control or kill a running game.
service.start('/z/SteamLinuxARM64/library', '/results', '/z');
failAudit = true; service.ticks = 24; state = 1; requests = [request()];
await service.tick(); assert.equal(service.sessions.length, 1);
failRecord = true; service.ticks = 49;
await service.tick(); assert.equal(service.sessions.length, 1);
failAudit = false; failRecord = false;
service.stop(); service.start('/z/SteamLinuxARM64/library', '/results', '/z');
replyAccepted = false; requests = [request()];
const registrationsBefore = launches.filter(call => call[0] === 'registerGame').length;
const stopBefore = stopped.length;
await service.tick();
assert.equal(service.sessions.length, 0, 'A disconnected Steam controller cannot leave an orphan game');
assert.equal(stopped.length, stopBefore + 1);
assert.equal(launches.filter(call => call[0] === 'registerGame').length, registrationsBefore);
replyAccepted = true; failLaunch = true; requests = [request()];
await service.tick(); assert.equal(service.sessions.length, 0, 'Per-request launch failure is contained');
failLaunch = false; requests = [request()]; await service.tick();
assert.equal(service.sessions.length, 1, 'Next launch works without restarting the broker');
service.stop();
const privateRoot = paths.LINUX_STEAM_PRIVATE_ROOT;
service.start(privateRoot + '/library', '/results', '/z');
const localGame = request();
localGame.argv = [privateRoot + '/library/steamapps/common/中文 游戏/game.exe',
  privateRoot + '/library/steamapps/common/中文 游戏/installscript.vdf', '/quiet', 'https://example.com/a', '-path=C:\\old'];
localGame.cwd = privateRoot + '/library/steamapps/common/中文 游戏';
requests = [localGame]; state = 1; await service.tick();
const privateLaunch = launches.filter(call => call[0] !== 'registerGame').at(-1);
assert.equal(privateLaunch[0], 'C:\\SteamLinuxARM64\\library\\steamapps\\common\\中文 游戏\\game.exe');
assert.deepEqual(Array.from(privateLaunch[1]), ['C:\\SteamLinuxARM64\\library\\steamapps\\common\\中文 游戏\\installscript.vdf',
  '/quiet', 'https://example.com/a', '-path=C:\\old']);
service.stop();
// A GNU ticket must never enter the OHOS Wine launcher, including failures.
let resolveGnu;
const gnuCancelled=[];
deps['libsmoke_napi.so'].default.startLinuxProtonGame = (id, language) => { assert.equal(language, 'zh_CN'); return new Promise(resolve => { resolveGnu=resolve; }); };
deps['libsmoke_napi.so'].default.cancelLinuxProtonGame = id => { gnuCancelled.push(id); return true; };
const taskHost=deps['./HostWindowTasks'].HostWindowTasks.getInstance();
taskHost.snapshot=()=>[]; taskHost.foregroundRevision=()=>0;
service.start('/z/SteamLinuxARM64/library','/results','/z');
const beforeGnu=launchCount();requests=[{...request(),id:30,backend:'gnu-proton'}];await service.tick();
assert.equal(runtimeLeases,1);assert.equal(launchCount(),beforeGnu);assert.equal(service.gnuSessions.length,1);
assert.equal(evidence().runtimeBackend,'gnu-proton');
requests=[{...request(),id:30,cancel:true}];await service.tick();assert.deepEqual(gnuCancelled,[30]);
resolveGnu(JSON.stringify({status:'proton_game_cancelled',childExitVerified:true,prefix:{exitCode:-1,signal:9,cancelled:true,childReaped:true}}));
await new Promise(resolve=>setImmediate(resolve));
assert.equal(runtimeLeases,0);assert.equal(service.gnuSessions.length,0);assert.equal(evidence().exitCode,130);
requests=[{...request(),id:31,backend:'gnu-proton-gl'}];await service.tick();
resolveGnu(JSON.stringify({status:'proton_package_missing_or_wrong_arch'}));await new Promise(resolve=>setImmediate(resolve));
assert.equal(launchCount(),beforeGnu,'A missing GNU runtime cannot silently run OHOS Wine');
assert.equal(evidence().runtimeBackend,'gnu-proton-gl');assert.equal(evidence().exitCode,1);
const fullGameResult=files.get('/results/gnu-proton-695630.json');
const lastGameResult=files.get('/results/last-game-launch.json');
requests=[{...query,id:33,backend:'gnu-proton'}];await service.tick();
resolveGnu(JSON.stringify({status:'proton_game_exited',childExitVerified:true,prefix:{exitCode:0,childReaped:true}}));
await new Promise(resolve=>setImmediate(resolve));
assert.equal(files.get('/results/gnu-proton-695630.json'),fullGameResult,'A GNU helper cannot overwrite the complete game result');
assert.equal(files.get('/results/last-game-launch.json'),lastGameResult,'A GNU helper cannot replace latest game status');
assert.equal(JSON.parse(files.get('/results/gnu-proton-helper-driver_query-695630.json')).prefix.exitCode,0);
requests=[{...request(),id:32,backend:'gnu-proton'}];await service.tick();service.stop();
assert.equal(runtimeLeases,1,'Stopping Steam cannot release a still-running game runtime');
resolveGnu(JSON.stringify({status:'proton_game_exited',childExitVerified:true,prefix:{exitCode:0,childReaped:true}}));await new Promise(resolve=>setImmediate(resolve));
assert.equal(runtimeLeases,0,'Child completion releases runtime even when broker generation changed');
assert.ok(gnuCancelled.includes(32),'Stopping Steam cancels its GNU session');
for (const bad of ['/z-other/SteamLinuxARM64', '/z/../outside/game.exe', '/outside/game.exe']) {
  assert.throws(() => paths.linuxSteamWindowsPath(bad, '/z'));
}
assert.equal(paths.linuxSteamWindowsPath('/z/one/../two', '/z'), 'Z:\\two');
console.log('Linux Steam Wine handoff, opt-in diagnostics, real exit status and unknown-state evidence: PASS');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
