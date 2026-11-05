#!/usr/bin/env node
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const compiledRoot = path.resolve(process.argv[2]);
const ts = require(path.resolve(process.argv[3]));
const models = require(path.join(compiledRoot, 'model/AppModels.js'));
const rules = require(path.join(compiledRoot, 'model/AppCatalogRules.js'));
const taskExports = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.resolve(__dirname,
  '../entry/src/main/ets/service/HostWindowTasks.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: taskExports });
const sourcePath = path.resolve(__dirname, '../entry/src/main/ets/service/AppSessionService.ets');
const compiled = ts.transpileModule(fs.readFileSync(sourcePath, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;

function fixture() {
  let state = models.EngineState.PREPARING;
  let root = 7;
  let rootThrows = false;
  const starts = [], launches = [];
  const globalSettings = models.createDefaultGlobalSettings();
  const appSettings = models.createDefaultAppSettings('cpu-test');
  const processes = [];
  const queries = { processes: 0 };
  const stopRequests = [], killRequests = [];
  let acceptStop = false;
  const native = {
    stopWineSession(id) { stopRequests.push(id); return acceptStop; },
    killProcess(pid) { killRequests.push(pid); return true; },
    getDesktopRootId() { if (rootThrows) throw new Error('native unavailable'); return root; },
    getWineSession() { return null; }, // Explorer is registered as @engine/explorer.
    getProcessList() { ++queries.processes; return processes.slice(); },
    activateWineSession() {},
    raiseToplevel() {}
  };
  const engine = {
    selectHostGraphicsBackendForExecutable: () => 'product-vulkan',
    needsHostGraphicsRestart: () => false,
    launchExecutable(...args) { launches.push(args);return {pid:42,sessionId:'cpu-test',reused:false}; },
    getState: () => state,
    isReady: () => state === models.EngineState.READY,
    getPresentationMode: () => models.WinePresentationMode.DESKTOP,
    subscribe: listener => listener(state)
  };
  const windows = { setSessionAssociationCallback() {}, getDesktopRootId:()=>root,
    isDesktopAbilityForeground:()=>false, usesEmbeddedDesktopHost:()=>false,
    resumeDesktopHost:async()=>{ starts.push({abilityName:'DesktopAbility',parameters:{toplevelId:root}}); } };
  const dependencies = {
    'libentry.so': { default: native },
    './LogService': { hilog: { info() {}, warn() {}, error() {} } },
    '../model/AppModels': models,
    '../model/AppCatalogRules': rules,
    './WineEngineService': { WineEngineService: { getInstance: () => engine } },
    './WineWindowManager': { WineWindowManager: { getInstance: () => windows } },
    './HostWindowTasks': taskExports,
    './AppSettingsStore': { AppSettingsStore: { getInstance: () => ({
      getGlobalSettings: () => globalSettings,
      getAppSettings: () => appSettings,
      recordRecentLaunch: async () => {}
    }) } },
    './InputOverlayWindowCoordinator': {},
    './AppCatalogService': {},
    '../steam/SteamLocalService': { SteamLocalService: { getInstance: () => ({ getCards: () => [], processesFor: () => [] }) } },
    '../steam/SteamLocalRules': {}
  };
  const exports = {};
  vm.runInNewContext(compiled, {
    exports,
    require(name) {
      assert.ok(Object.hasOwn(dependencies, name), `Unexpected dependency: ${name}`);
      return dependencies[name];
    }
  }, { filename: sourcePath });
  const service = new exports.AppSessionService();
  service.initialize({ startAbility: async want => { starts.push(want); } });
  return {
    service, engine, starts, launches, globalSettings, appSettings, processes, queries, stopRequests, killRequests,
    acceptStop: () => { acceptStop = true; },
    setState: next => { state = next; },
    setRoot: next => { root = next; },
    failRootQuery: () => { rootThrows = true; }
  };
}

(async () => {
  const precisionDefaults = models.createDefaultAppSettings('precision');
  assert.equal(precisionDefaults.fexStrictPrecision, false);
  assert.deepEqual(models.resolveFexCompatEnv('fex', precisionDefaults), []);
  const oldSettings = { ...precisionDefaults };
  delete oldSettings.fexStrictPrecision;
  assert.equal(models.cloneAppSettings(oldSettings).fexStrictPrecision, false);
  const strictSettings = models.cloneAppSettings(precisionDefaults);
  strictSettings.fexStrictPrecision = true;
  assert.deepEqual(models.resolveFexCompatEnv('fex', strictSettings), ['FEX_X87REDUCEDPRECISION=0']);
  assert.deepEqual(models.resolveFexCompatEnv('box64', strictSettings), []);
  assert.equal(models.cloneAppSettings(JSON.parse(JSON.stringify(strictSettings))).fexStrictPrecision, true);

  assert.equal(precisionDefaults.wineTranslationEngine, 'default');
  const oldEngineSettings = { ...precisionDefaults };
  delete oldEngineSettings.wineTranslationEngine;
  assert.equal(models.cloneAppSettings(oldEngineSettings).wineTranslationEngine, 'default');
  assert.equal(models.normalizeAppWineTranslationEngine('invalid'), 'default');
  for (const globalEngine of ['fex','box64']) for (const appEngine of ['default','fex','box64']) {
    const engine = appEngine === 'default' ? globalEngine : appEngine;
    assert.equal(models.resolveAppWineTranslationEngine(globalEngine, appEngine), engine);
    const f=fixture();f.setState(models.EngineState.READY);f.engine.getPresentationMode=()=>models.WinePresentationMode.MANAGED_WINDOWS;
    f.globalSettings.wineTranslationEngine=globalEngine;f.appSettings.wineTranslationEngine=appEngine;f.globalSettings.box64Preset='performance';
    f.appSettings.box64Preset='unity';f.appSettings.disableMfDxgiDeviceManager=true;
    f.appSettings.fexStrictPrecision=true;
    f.processes.push({pid:42,path:'C:/game.exe',state:'running'});
    const app={id:'cpu-test',launchKind:models.LaunchKind.WINE_EXE,
      launchTarget:{executable:'C:/game.exe',arguments:[],workingDirectory:''}};
    await f.service.launchReady(app,f.globalSettings,models.DisplayMode.DESKTOP,models.WinePresentationMode.MANAGED_WINDOWS,'/games');
    assert.equal(f.launches.length,1);
    const [,argv,,env]=f.launches[0];
    assert.ok(env.includes('WINEHUA_WOW64_ENGINE=' + (engine==='box64'?'box':'fex')),
      'Every app launch carries its resolved engine, overriding the desktop global choice');
    assert.equal(models.cloneAppSettings(JSON.parse(JSON.stringify(f.appSettings))).wineTranslationEngine, appEngine);
    const box=Array.from(env).filter(x=>x.startsWith('BOX64_'));
    assert.equal(JSON.stringify(box),JSON.stringify(engine==='box64'?models.resolveBox64PresetEnv('unity'):[]));
    assert.equal(argv.includes('-force-gfx-direct'),engine==='box64');
    assert.ok(env.includes(models.MF_DXGI_DEVICE_MANAGER_DISABLE_ENV),'MF compatibility remains independent of CPU selection');
    assert.equal(env.includes('FEX_X87REDUCEDPRECISION=0'), engine==='fex',
      'Strict x87 is delivered to the real game launch only under FEX');
  }

  // Real phone order: root callback, catalog reconciliation, then engine READY.
  // No fabricated native desktop process entry may be needed to keep the root.
  const f = fixture();
  f.service.associateToplevel('desktop', 0, 7);
  assert.ok(f.service.getSession('desktop'), 'PREPARING reconciliation lost the live desktop root');
  assert.ok(f.service.getRunningAppIds().has('builtin-desktop'));
  f.setState(models.EngineState.READY);
  const session = f.service.getSession('desktop');
  assert.equal(session.toplevelId, 7);
  await f.service.resume(session);
  assert.equal(f.starts.length, 1);
  assert.equal(f.starts[0].abilityName, 'DesktopAbility');
  assert.equal(f.starts[0].parameters.toplevelId, 7);

  for (const state of [models.EngineState.STOPPED, models.EngineState.ERROR,
    models.EngineState.SWITCHING]) {
    const stopped = fixture();
    stopped.service.associateToplevel('desktop', 0, 7);
    stopped.setState(state);
    assert.equal(stopped.service.getSession('desktop'), null, `Stale root retained in ${state}`);
  }
  for (const root of [0, 8]) {
    const stale = fixture();
    stale.service.associateToplevel('desktop', 0, 7);
    stale.setRoot(root);
    assert.equal(stale.service.getSession('desktop'), null, 'Wrong/dead root must still be removed');
  }
  const unavailable = fixture();
  unavailable.service.associateToplevel('desktop', 0, 7);
  unavailable.failRootQuery();
  assert.equal(unavailable.service.getSession('desktop'), null);

  const deadProcess = fixture();
  deadProcess.service.registerDiagnosticLaunch('C:/game.exe', { pid: 42, sessionId: 'game' },
    models.DisplayMode.DESKTOP, models.WinePresentationMode.DESKTOP);
  assert.equal(deadProcess.service.getSession('game'), null, 'Dead game PID must still be reconciled');

  const running = fixture();
  running.setState(models.EngineState.READY);
  running.processes.push({ pid: 42, path: 'C:/game.exe', state: 'running' },
    { pid: 77, path: 'C:/native-only.exe', state: 'running' },
    { pid: 78, path: 'C:/windows/system32/services.exe', state: 'running' });
  running.service.registerDiagnosticLaunch('C:/game.exe', { pid: 42, sessionId: 'game' },
    models.DisplayMode.DESKTOP, models.WinePresentationMode.DESKTOP);
  const runningSession = running.service.getSession('game');
  assert.equal(running.service.stop(runningSession), false);
  assert.ok(running.service.getSession('game'), 'Rejected stop must preserve the running session');
  assert.deepEqual(running.killRequests, [], 'Exact session rejection must not fall back to a recycled PID');
  running.acceptStop();
  assert.equal(running.service.stop(runningSession), true);
  assert.ok(running.service.getSession('game'), 'Accepted request must await authoritative process exit');
  let before = running.queries.processes;
  const snapshot = running.service.getRunningSnapshot();
  assert.equal(running.queries.processes - before, 1, 'A UI snapshot must query native processes once');
  assert.ok(snapshot.paths.has(models.normalizeLaunchPath('C:/game.exe')));
  assert.ok(snapshot.paths.has(models.normalizeLaunchPath('C:/native-only.exe')),
    'Native-only launches must remain visible to catalog path matching');
  assert.ok(!snapshot.paths.has(models.normalizeLaunchPath('C:/windows/system32/services.exe')),
    'Wine infrastructure must not appear as a catalog game');
  assert.equal(snapshot.appIds.size, 1);
  running.processes.splice(0, running.processes.length);
  before = running.queries.processes;
  const exited = running.service.getRunningSnapshot();
  assert.equal(running.queries.processes - before, 1);
  assert.equal(exited.appIds.size, 0, 'Process exits must be visible on the next snapshot');
  assert.equal(exited.paths.size, 0, 'Exited native-only launches must not be cached');
  const reused = fixture();
  reused.setState(models.EngineState.READY);
  reused.processes.push({ pid: 42, path: 'C:/game.exe', state: 'running', sessionId: 'wine-42-1' });
  reused.service.registerDiagnosticLaunch('C:/game.exe', { pid: 42, sessionId: 'wine-42-1' },
    models.DisplayMode.DESKTOP, models.WinePresentationMode.DESKTOP);
  assert.ok(reused.service.getSession('wine-42-1'));
  reused.processes[0].sessionId = 'wine-42-2';
  assert.equal(reused.service.getSession('wine-42-1'), null, 'Recycled PID cannot preserve a stale UI session');
  const external = fixture();
  external.setState(models.EngineState.READY);
  external.processes.push({ pid: 84, path: 'Z:/SteamLinuxARM64/library/game.exe', state: 'running', sessionId: 'external-84' });
  external.service.associateToplevel('external-84', 84, 99); // Window before registration.
  const registered = external.service.registerExternalLaunch('steam-linux-game-695630',
    'Z:/SteamLinuxARM64/library/game.exe', { pid: 84, sessionId: 'external-84' },
    models.DisplayMode.DESKTOP, models.WinePresentationMode.MANAGED_WINDOWS);
  assert.equal(registered.toplevelId, 99);
  assert.equal(registered.appId, 'steam-linux-game-695630');
  assert.equal(external.service.registerExternalLaunch('ignored', registered.executable,
    { pid: 84, sessionId: 'external-84' }, registered.displayMode, registered.presentationMode), registered);
  taskExports.HostWindowTasks.getInstance().onEvent(99, 'created', JSON.stringify({ sessionId: 'external-84' }));
  let resumed = 0;
  taskExports.HostWindowTasks.getInstance().setActivator(async () => { ++resumed; return true; });
  await external.service.resume(registered);
  assert.equal(resumed, 1, 'External managed game uses the shared restore/focus operation');
  assert.equal(external.starts.length, 0, 'Resume must not create another Ability/process');
  const background = fixture();
  background.setState(models.EngineState.READY);
  background.processes.push({pid:88,path:'C:/hmosdeck/hmosdeck-steam.exe',state:'running',sessionId:'steam-host'});
  background.service.associateToplevel('steam-host',88,100);
  let promotes = 0;
  background.service.resume = async () => {promotes++;};
  background.service.registerExternalLaunch('steam-linux-client', 'C:/hmosdeck/hmosdeck-steam.exe',
    {pid:88,sessionId:'steam-host'}, models.DisplayMode.DESKTOP, models.WinePresentationMode.DESKTOP, false);
  background.service.associateToplevel('steam-host',88,101);
  assert.equal(promotes,0,'External Steam owner must not auto-promote on early or replacement windows');
  background.service.registerExternalLaunch('ordinary-game', 'C:/game.exe',
    {pid:89,sessionId:'ordinary'}, models.DisplayMode.DESKTOP, models.WinePresentationMode.DESKTOP);
  background.service.schedulePromoteRetry=()=>{};
  background.service.associateToplevel('ordinary',89,102);
  assert.equal(promotes,1,'Ordinary Wine launch keeps its existing automatic presentation');
  console.log('AppSession startup/root, stop acceptance and process-incarnation reconciliation: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
