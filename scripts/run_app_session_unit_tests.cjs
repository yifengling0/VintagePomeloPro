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
  const starts = [], launches = [], gameTargets = [];
  const globalSettings = models.createDefaultGlobalSettings();
  const appSettings = models.createDefaultAppSettings('cpu-test');
  const processes = [];
  const queries = { processes: 0 };
  const stopRequests = [], killRequests = [];
  let acceptStop = false;
  let gameEnabled = false, nativeSession = null, queryThrows = false;
  const timers = new Map();
  let clock = 100000, waitAction = () => {};
  const steamCards = [];
  const native = {
    stopWineSession(id) { stopRequests.push(id); return acceptStop; },
    killProcess(pid) { killRequests.push(pid); return true; },
    getDesktopRootId() { if (rootThrows) throw new Error('native unavailable'); return root; },
    getWineSession() { if (queryThrows) throw new Error('native unavailable'); return nativeSession; },
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
  const steam = { canLaunch: () => true, hasForeignClient: () => false, prepareLaunch: async () => {},
    isCompatibility: () => false, getClientDirectory: () => 'C:/Steam',
    clientCard: () => ({ id: 'steam-client-default', launchTarget: { executable: 'C:/Steam/steam.exe' } }),
    getCards: () => steamCards,
    processesFor: (app, processes, includeUi = false) => processes.filter(p => p.state === 'running' &&
      (p.path === app.launchTarget.executable || (includeUi && p.path === 'C:/Steam/bin/cef/cef.win7x64/steamwebhelper.exe'))) };
  const dependencies = {
    'libentry.so': { default: native },
    './LogService': { hilog: { info() {}, warn() {}, error() {} } },
    '../model/AppModels': models,
    '../model/GraphicsBackendSettings': require(path.join(compiledRoot, 'model/GraphicsBackendSettings.js')),
    '../model/AppCatalogRules': rules,
    './GameModeService': { GameModeService: { begin() {}, finish() {}, reset() {},
      present: id => { gameTargets.push(id); return gameEnabled; },
      presentDesktop: () => { gameTargets.push(0); return gameEnabled; },
      enabled: () => gameEnabled, noteActiveApp() {} } },
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
    '../steam/SteamLocalService': { SteamLocalService: { getInstance: () => steam } },
    '../steam/SteamLocalRules': { STEAM_CLIENT_ID: 'steam-client-default', steamLaunchArguments: () => ['steam://open/games'] },
    '@ohos.file.fs': { default: { mkdirSync() {}, OpenMode: { CREATE: 1, WRITE_ONLY: 2, TRUNC: 4 }, openSync() { return { fd: 1 }; }, writeSync() {}, closeSync() {} } },
    './GameContainer': { DEFAULT_GAME_CONTAINER_ID: 'default', wineDriveCForContainer: () => '/data/storage/el2/base/files/.wine/drive_c' }
  };
  const exports = {};
  vm.runInNewContext(compiled, {
    exports,
    Date: { now: () => clock },
    setTimeout: (action, delay) => { clock += delay; waitAction(); queueMicrotask(action); return 1; },
    clearTimeout() {},
    setInterval: action => { const id = timers.size + 1; timers.set(id, action); return id; },
    clearInterval: id => timers.delete(id),
    require(name) {
      assert.ok(Object.hasOwn(dependencies, name), `Unexpected dependency: ${name}`);
      return dependencies[name];
    }
  }, { filename: sourcePath });
  const service = new exports.AppSessionService();
  service.initialize({ startAbility: async want => { starts.push(want); } });
  return {
    service, engine, starts, launches, gameTargets, globalSettings, appSettings, processes, queries, stopRequests, killRequests,
    acceptStop: () => { acceptStop = true; },
    setState: next => { state = next; },
    setRoot: next => { root = next; },
    failRootQuery: () => { rootThrows = true; },
    enableGame: () => { gameEnabled = true; },
    setNativeSession: value => { nativeSession = value; },
    failProcessQuery: value => { queryThrows = value; },
    timers, steam, steamCards, onWait: action => { waitAction = action; }
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
  assert.equal(precisionDefaults.dxvkFloatControls, true);
  const floatOff = models.cloneAppSettings(precisionDefaults);
  floatOff.dxvkFloatControls = false;
  assert.equal(models.cloneAppSettings(JSON.parse(JSON.stringify(floatOff))).dxvkFloatControls, false);
  const oldFloat = { ...precisionDefaults };
  delete oldFloat.dxvkFloatControls;
  assert.equal(models.cloneAppSettings(oldFloat).dxvkFloatControls, true);

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

  const floatApp = {id:'cpu-test',launchKind:models.LaunchKind.WINE_EXE,
    launchTarget:{executable:'C:/game.exe',arguments:[],workingDirectory:''}};
  for (const current of [[], ['-video_app','direct3d9','-video_multisample','0','-video_vsync','0']]) {
    const f=fixture(); f.setState(models.EngineState.READY);
    f.engine.getPresentationMode=()=>models.WinePresentationMode.MANAGED_WINDOWS;
    f.appSettings.arguments=current;
    const app={...floatApp,source:models.AppSource.DOWNLOAD,
      launchTarget:{...floatApp.launchTarget,arguments:['-video_app','direct3d11']}};
    await f.service.launchReady(app,f.globalSettings,models.DisplayMode.DESKTOP,
      models.WinePresentationMode.MANAGED_WINDOWS,'/games');
    assert.deepEqual(Array.from(f.launches[0][1]),current,
      'Latest local settings replace stale catalog arguments, even when cleared');
  }
  const floatLaunchOff = fixture();
  floatLaunchOff.setState(models.EngineState.READY);
  floatLaunchOff.engine.getPresentationMode = () => models.WinePresentationMode.MANAGED_WINDOWS;
  floatLaunchOff.appSettings.dxvkFloatControls = false;
  floatLaunchOff.processes.push({pid:42,path:'C:/game.exe',state:'running'});
  await floatLaunchOff.service.launchReady(floatApp, floatLaunchOff.globalSettings,
    models.DisplayMode.DESKTOP, models.WinePresentationMode.MANAGED_WINDOWS, '/games');
  assert.ok(floatLaunchOff.launches[0][3].includes(
    'DXVK_CONFIG_FILE=C:\\users\\steamuser\\Documents\\vpp-float-controls.conf'),
    'floatControls off injects the per-game DXVK config file');

  const floatLaunchOn = fixture();
  floatLaunchOn.setState(models.EngineState.READY);
  floatLaunchOn.engine.getPresentationMode = () => models.WinePresentationMode.MANAGED_WINDOWS;
  floatLaunchOn.processes.push({pid:42,path:'C:/game.exe',state:'running'});
  await floatLaunchOn.service.launchReady(floatApp, floatLaunchOn.globalSettings,
    models.DisplayMode.DESKTOP, models.WinePresentationMode.MANAGED_WINDOWS, '/games');
  assert.ok(!floatLaunchOn.launches[0][3].some(x => x.startsWith('DXVK_CONFIG_FILE=')),
    'floatControls on keeps the DXVK default');

  const nestedApp = {id:'cpu-test',launchKind:models.LaunchKind.WINE_EXE,directory:'/games/PAL4',
    launchTarget:{executable:'/games/PAL4/PAL4/pal4.exe',arguments:[],workingDirectory:'/games/PAL4'}};
  const nested = fixture();
  nested.setState(models.EngineState.READY);
  nested.engine.getPresentationMode = () => models.WinePresentationMode.MANAGED_WINDOWS;
  nested.processes.push({pid:42,path:'/games/PAL4/PAL4/pal4.exe',state:'running'});
  await nested.service.launchReady(nestedApp, nested.globalSettings, models.DisplayMode.DESKTOP,
    models.WinePresentationMode.MANAGED_WINDOWS, '/games');
  assert.equal(nested.launches[0][2], '/games/PAL4/PAL4',
    'stale game-root working directory is corrected to the executable directory');

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
  { // Return only after both the real process and all its windows are gone.
    const f = fixture(); f.enableGame(); f.setState(models.EngineState.READY);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markDesktopForeground();
    let returns = 0; tasks.setLauncher(async () => { ++returns; });
    const session = { sessionId: 'game-return', appId: 'pal4', pid: 42, toplevelId: 70,
      executable: 'C:/pal4.exe', state: models.SessionState.RUNNING,
      presentationMode: models.WinePresentationMode.DESKTOP };
    tasks.onEvent(70, 'created', JSON.stringify({ sessionId: session.sessionId }));
    f.service.presentGameSession(session);
    assert.equal(f.timers.size, 1);
    f.setNativeSession({ state: 'running' });
    tasks.onEvent(70, 'destroyed', '{}');
    f.service.checkGameReturn(); assert.equal(returns, 0, 'Splash teardown is not process exit');
    f.setNativeSession(null); f.failProcessQuery(true);
    f.service.checkGameReturn(); assert.equal(returns, 0, 'Query failure retains the game');
    f.failProcessQuery(false);
    f.processes.push({ pid: 43, sessionId: session.sessionId, state: 'running' });
    f.service.checkGameReturn(); assert.equal(returns, 0, 'A surviving child retains the game session');
    f.processes.length = 0;
    tasks.onEvent(71, 'created', JSON.stringify({ sessionId: session.sessionId }));
    f.service.checkGameReturn(); assert.equal(returns, 0, 'Process exit waits for its final window');
    tasks.onEvent(71, 'destroyed', '{}');
    f.service.checkGameReturn(); assert.equal(returns, 1);
    f.service.checkGameReturn(); assert.equal(returns, 1, 'Exit returns once');
    assert.equal(f.timers.size, 0);
    tasks.markDesktopForeground(); f.service.presentGameSession(session); tasks.markLauncherForeground();
    f.service.checkGameReturn(); assert.equal(returns, 1, 'Explicit library navigation wins');
    tasks.markDesktopForeground(); f.service.presentGameSession(session);
    tasks.onEvent(80, 'created', JSON.stringify({ sessionId: 'another-game' }));
    f.service.checkGameReturn(); assert.equal(returns, 1, 'Another foreground game wins');
    tasks.reset(); tasks.markDesktopForeground();
    const client = { ...session, sessionId: 'steam-client', appId: 'steam-client-default' };
    f.service.sessions.set(client.sessionId, client);
    let resumed = null; f.service.resume = async value => { resumed = value; };
    f.service.presentGameSession({ ...session, appId: 'steam-default-123' });
    f.service.checkGameReturn();
    assert.equal(resumed, client, 'Steam-dispatched game returns to its existing client');
    assert.equal(returns, 1);
  }
  console.log('AppSession startup/root, stop acceptance, process reconciliation and Game exit return: PASS');
  const steamApp = { id: 'steam-client-default', steam: { state: 'client' },
    launchTarget: { executable: 'C:/Steam/steam.exe', arguments: [], workingDirectory: 'C:/Steam' } };
  const launched = { appId: steamApp.id, sessionId: 'sender', pid: 42, toplevelId: 0 };
  { // A replaced CEF window stays part of the desktop shell, not a game crop.
    const f = fixture(); f.enableGame(); f.setState(models.EngineState.READY);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset();
    f.processes.push({ pid: 82, sessionId: 'real-client', path: 'C:/Steam/steam.exe', state: 'running' },
      { pid: 83, sessionId: 'cef-browser', path: 'C:/Steam/bin/cef/cef.win7x64/steamwebhelper.exe', state: 'running' });
    const client = { ...launched, executable: 'C:/Steam/steam.exe', sessionId: 'real-client', pid: 82, toplevelId: 94,
      presentationMode: models.WinePresentationMode.DESKTOP, state: models.SessionState.RUNNING };
    tasks.onEvent(94, 'created', JSON.stringify({ sessionId: 'cef-browser', clientPid: 83 }));
    tasks.onEvent(94, 'destroyed', '{}');
    tasks.onEvent(95, 'created', JSON.stringify({ sessionId: 'cef-browser', clientPid: 83 }));
    await f.service.resume(client);
    assert.equal(client.toplevelId, 95, 'Return resolves the replacement CEF window');
    assert.equal(client.sessionId, 'real-client', 'Keep the actual Steam process identity');
    assert.deepEqual(f.gameTargets, [0], 'Steam retains multiwindow desktop coordinates');
    assert.equal(f.timers.size, 1, 'Normal Steam exit still returns to Game library');
  }
  { // Cold launch registers its carrier but never presents an empty desktop.
    const f = fixture(); f.setState(models.EngineState.READY);
    f.service.associateToplevel('desktop', 0, 7);
    const result = await f.service.launchReady(steamApp, f.globalSettings, models.DisplayMode.DESKTOP,
      models.WinePresentationMode.DESKTOP, '/games');
    assert.equal(f.launches.length, 1); assert.equal(f.starts.length, 0);
    assert.equal(result.automaticForeground, false);
    f.steam.prepareLaunch = async () => { throw Error('Steam GPU dependency failed'); };
    await assert.rejects(f.service.launchReady(steamApp, f.globalSettings, models.DisplayMode.DESKTOP,
      models.WinePresentationMode.DESKTOP, '/games'), /GPU dependency failed/);
    assert.equal(f.launches.length, 1); assert.equal(f.starts.length, 0);
  }
  { // Desktop and a same-title unrelated app cannot satisfy Steam readiness.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    tasks.onEvent(7, 'desktop_root', '{}');
    tasks.onEvent(90, 'created', JSON.stringify({ sessionId: 'other', clientPid: 999 }));
    tasks.onEvent(90, 'title', JSON.stringify({ title: 'Steam' }));
    f.processes.push({ pid: 42, sessionId: 'sender', path: 'C:/Steam/steam.exe', state: 'running' });
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial()), /未显示客户端窗口/);
    assert.equal(f.starts.length, 0, 'Do not show an empty desktop as successful startup');
    assert.equal(f.killRequests.length, 0, 'Timeout never kills the imported client');
    assert.equal(f.timers.size, 0, 'No background polling leaks after timeout');
  }
  { // Bootstrap exits; the real client/window belongs to a new native session.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    let waits = 0, resumed = null;
    f.service.resume = async session => { resumed = session; };
    f.onWait(() => {
      if (++waits !== 2) return;
      f.processes.push({ pid: 82, sessionId: 'real-client', path: 'C:/Steam/steam.exe', state: 'running' });
      tasks.onEvent(91, 'created', JSON.stringify({ sessionId: 'real-client', clientPid: 82 }));
    });
    const ready = await f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial());
    assert.equal(ready.pid, 82); assert.equal(ready.sessionId, 'real-client');
    assert.equal(resumed, ready); assert.equal(ready.toplevelId, 91);
    assert.equal(waits, 2); assert.equal(ready.automaticForeground, true);
  }
  { // The managed CEF window belongs to another native session than steam.exe.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    f.processes.push({ pid: 82, sessionId: 'real-client', path: 'C:/Steam/steam.exe', state: 'running' },
      { pid: 83, sessionId: 'cef-browser', path: 'C:/Steam/bin/cef/cef.win7x64/steamwebhelper.exe', state: 'running' });
    tasks.onEvent(94, 'created', JSON.stringify({ sessionId: 'cef-browser', clientPid: 83 }));
    let resumed; f.service.resume = async session => { resumed = session; };
    const ready = await f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial());
    assert.equal(ready.sessionId, 'real-client'); assert.equal(ready.pid, 82);
    assert.equal(ready.toplevelId, 94); assert.equal(resumed, ready);
    assert.equal(f.service.sessions.size, 1, 'Do not register CEF as a separate game or client');
    assert.equal(f.service.steamClientWindowKey(steamApp), tasks.snapshot().find(task => task.toplevelId === 94).key);
    f.processes[1].sessionId = 'recycled-cef';
    assert.equal(f.service.steamClientWindowKey(steamApp), '', 'A recycled CEF PID cannot restore an old window');
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial()), /未显示客户端窗口/);
    f.processes[1].sessionId = 'cef-browser'; f.processes[0].state = 'stopped';
    assert.equal(f.service.steamClientWindowKey(steamApp), '', 'A helper alone does not represent a live Steam client');
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial()), /未显示客户端窗口/);
  }
  { // A slow translated CEF start must still bring the real client forward.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    f.processes.push({ pid: 82, sessionId: 'slow-client', path: 'C:/Steam/steam.exe', state: 'running' },
      { pid: 83, sessionId: 'slow-cef', path: 'C:/Steam/bin/cef/cef.win7x64/steamwebhelper.exe', state: 'running' });
    let elapsed = 0, resumed;
    f.service.resume = async session => { resumed = session; };
    f.onWait(() => {
      elapsed += 250;
      if (elapsed === 60000) tasks.onEvent(96, 'created', JSON.stringify({ sessionId: 'slow-cef', clientPid: 83 }));
    });
    const ready = await f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial());
    assert.equal(elapsed, 60000); assert.equal(ready.toplevelId, 96); assert.equal(resumed, ready);
    assert.equal(ready.sessionId, 'slow-client');
    assert.equal(f.killRequests.length, 0);
  }
  { // A missing window still has a bounded wait without terminating Steam.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    let elapsed = 0;
    f.onWait(() => { elapsed += 250; });
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial()), /未显示客户端窗口/);
    assert.ok(elapsed > 60000 && elapsed <= 90000);
    assert.equal(f.starts.length, 0); assert.equal(f.killRequests.length, 0);
    assert.equal(f.timers.size, 0);
  }
  { // Warm URI forwarding uses the existing real client, respecting navigation.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    const navigation = tasks.navigationSerial(); tasks.markLauncherForeground();
    f.processes.push({ pid: 82, sessionId: 'warm-client', path: 'C:/Steam/steam.exe', state: 'running' });
    tasks.onEvent(92, 'created', JSON.stringify({ sessionId: 'warm-client', clientPid: 82 }));
    let resumed = false; f.service.resume = async () => { resumed = true; };
    assert.equal((await f.service.waitForSteamClientWindow(steamApp, launched, navigation)).pid, 82);
    assert.equal(resumed, false, 'A later user navigation wins over startup auto-focus');
    f.setState(models.EngineState.ERROR);
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, navigation), /会话已停止/);
  }
  { // Queued setup errors retain their cause rather than silently returning null.
    const f = fixture(); f.setState(models.EngineState.READY);
    let failure;
    f.service.launchReady = async () => { throw Error('Steam helper is unavailable'); };
    f.service.pendingLaunches.push({ app: steamApp, settings: f.globalSettings,
      presentationMode: models.WinePresentationMode.DESKTOP, resolve() { assert.fail('Failure was swallowed'); },
      reject: error => { failure = error; } });
    f.service.drainPendingLaunches(); await Promise.resolve(); await Promise.resolve();
    assert.match(failure.message, /helper is unavailable/);
  }
  { // A recycled PID at the same Steam path cannot claim an old live window.
    const f = fixture(); f.setState(models.EngineState.READY); f.steamCards.push(steamApp);
    const tasks = taskExports.HostWindowTasks.getInstance(); tasks.reset(); tasks.markLauncherForeground();
    f.processes.push({ pid: 42, sessionId: 'new-incarnation', path: 'C:/Steam/steam.exe', state: 'running' });
    tasks.onEvent(93, 'created', JSON.stringify({ sessionId: 'sender', clientPid: 42 }));
    await assert.rejects(f.service.waitForSteamClientWindow(steamApp, launched, tasks.navigationSerial()), /未显示客户端窗口/);
    assert.equal(f.starts.length, 0);
  }
  console.log('Steam real-window readiness, bootstrap handoff, warm navigation, bounded timeout and error propagation: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
