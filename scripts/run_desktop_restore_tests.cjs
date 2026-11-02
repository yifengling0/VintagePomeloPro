#!/usr/bin/env node
// Host-only regression: real ETS orchestration, mocked OS/native boundaries.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const ts = require(process.argv[2]);
const root = path.resolve(__dirname, '../entry/src/main/ets');
function load(file, deps = {}, globals = {}) {
  const source = fs.readFileSync(path.join(root, file + '.ets'), 'utf8');
  const exports = {};
  vm.runInNewContext(ts.transpileModule(source, {compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  }}).outputText, {exports, setTimeout, clearTimeout, ...globals, require(name) {
    assert.ok(Object.hasOwn(deps, name), 'missing dependency: ' + name);
    return deps[name];
  }}, {filename: file + '.ets'});
  return exports;
}
const models = load('model/AppModels', {
  '../common/EvdevKeyNames': load('common/EvdevKeyNames'),
  './PerformanceHudSettings': load('model/PerformanceHudSettings')
});
const rules = load('model/AppCatalogRules');
const hilog = {info(){},warn(){},error(){}};
const indexSource = fs.readFileSync(path.join(root,'pages/Index.ets'),'utf8');
const indexMethods = ['showDesktopLayer','onDesktopEnterFullscreenChange'].map(name => {
  const found = indexSource.match(new RegExp('private '+name+'\\(\\): [a-z]+ \\{[\\s\\S]*?\\n  \\}'));
  assert.ok(found,'Index method missing: '+name);
  return found[0];
});

async function fixture(deviceType, pcMode, initialMode, laterMode=initialMode, laterPcMode=pcMode) {
  const values = new Map(), starts = [], nativeCalls = [];
  const AppStorage = {get:key=>values.get(key),setOrCreate:(key,value)=>values.set(key,value)};
  const globals = {AppStorage};
  let currentPcMode = pcMode;
  const policy = load('common/DeviceCapabilityPolicy', {
    '../model/ValueAddedModels':{},'@ohos.deviceInfo':{default:{deviceType}},
    '@kit.BasicServicesKit':{settings:{domainName:{USER_PROPERTY:'user'},getValueSync:()=>currentPcMode}},
    '@kit.ArkUI':{window:{}},'../service/LogService':{hilog},'../model/AppModels':models
  },globals).DeviceCapabilityPolicy;
  policy.refreshPcMode({});
  const settings = models.createDefaultGlobalSettings();
  settings.desktopWindowMode = initialMode;
  const store = {getGlobalSettings:()=>settings,usesManagedWineWindows:()=>policy.usesManagedWineWindows(),
    getAppSettings:()=>models.createDefaultAppSettings('game'),recordRecentLaunch:async()=>{}};
  const hostTasks = {reset(){},onEvent(){},setWineGameEligibility(){},markDesktopForeground(){},associateSession(){}};
  const processes = [{pid:42,path:'C:/game.exe',state:'running',sessionId:'game'}];
  const native = {setDesktopMode(){},getDesktopRootId:()=>7,getWineSession:()=>null,
    getProcessList:()=>processes,activateWineSession:sid=>nativeCalls.push(['activate',sid]),
    raiseToplevel:id=>nativeCalls.push(['raise',id])};
  const wm = load('service/WineWindowManager', {
    '@kit.AbilityKit':{},'@kit.ArkUI':{window:{}},'./LogService':{hilog},
    '@kit.BasicServicesKit':{},'@kit.InputKit':{},'libentry.so':{default:native},
    'libsmoke_napi.so':{default:{}},'./HostWindowTasks':{HostWindowTasks:{getInstance:()=>hostTasks}},
    './InputDispatcher':{},'../model/AppModels':models,'./PopupWindowManager':{},
    './ModalWindowManager':{},'./FusionWindowManager':{},
    './AppSettingsStore':{AppSettingsStore:{getInstance:()=>store}},'./DesktopModeService':{},
    '../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:policy}
  },globals).WineWindowManager.getInstance();
  const context = {startAbility:async want=>{starts.push(want);}};
  wm.uiAbilityContext = context;
  wm.applyPointerVisibility = () => {}; // Native pointer API is outside carrier selection.
  wm.setPresentationMode(models.WinePresentationMode.DESKTOP);
  wm.onToplevelEvent(7,'desktop_root','{}');
  if (wm.desktopAbilityRequest) await wm.desktopAbilityRequest;
  const embedded = wm.usesEmbeddedDesktopHost();
  assert.equal(starts.length,embedded?0:1,'initial launch selects one carrier');
  if (!embedded) {
    wm.markDesktopAbilityForeground(7);
    wm.markDesktopAbilityBackground(7);
    wm.markDesktopAbilityStopped(7);
  }
  assert.equal(wm.getDesktopRootId(),7,'closing carrier preserves root');
  const engine = {getState:()=>models.EngineState.READY,isReady:()=>true,
    getMode:()=>models.DisplayMode.DESKTOP,getPresentationMode:()=>models.WinePresentationMode.DESKTOP,
    subscribe(){},ensureReady:async()=>true};
  const service = new (load('service/AppSessionService', {
    'libentry.so':{default:native},'./LogService':{hilog},'../model/AppModels':models,
    '../model/AppCatalogRules':rules,'./WineEngineService':{WineEngineService:{getInstance:()=>engine}},
    './WineWindowManager':{WineWindowManager:{getInstance:()=>wm}},
    './HostWindowTasks':{HostWindowTasks:{getInstance:()=>hostTasks}},
    './AppSettingsStore':{AppSettingsStore:{getInstance:()=>store}},'./InputOverlayWindowCoordinator':{},
    './AppCatalogService':{},'../steam/SteamLocalService':{SteamLocalService:{getInstance:()=>({
      getCards:()=>[],processesFor:()=>[]})}},'../steam/SteamLocalRules':{}
  },globals).AppSessionService)();
  service.initialize(context);
  settings.desktopWindowMode=laterMode;currentPcMode=laterPcMode;policy.refreshPcMode({});
  const desktop = {sessionId:'desktop',appId:'builtin-desktop',toplevelId:7,
    presentationMode:models.WinePresentationMode.DESKTOP};
  const child = {sessionId:'game',appId:'game',pid:42,executable:'C:/game.exe',toplevelId:9,
    presentationMode:models.WinePresentationMode.DESKTOP,state:models.SessionState.RUNNING};
  const exports = {};
  vm.runInNewContext(ts.transpileModule('export class IndexProbe {'+indexMethods.join('\n')+'}', {
    compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}
  }).outputText,{exports,AppStorage,DeviceCapabilityPolicy:policy,DesktopWindowMode:models.DesktopWindowMode,
    WineWindowManager:{getInstance:()=>wm}});
  const index = new exports.IndexProbe();
  Object.assign(index,{desktopEmbeddedHost:embedded,settings,desktopActive:true,launcherVisible:true,
    setLauncherVisible(value){this.launcherVisible=value;}});
  function checkCarrier() {
    index.desktopEnterFullscreen=values.get('desktopEnterFullscreen')===true;
    assert.equal(index.desktopEnterFullscreen,embedded,
      `restore signal: mode=${wm.isDesktopMode}, root=${wm.getDesktopRootId()}, embedded=${wm.usesEmbeddedDesktopHost()}`);
    index.onDesktopEnterFullscreenChange();
    assert.equal(index.launcherVisible,!embedded,
      `${deviceType}/${pcMode}/${initialMode}->${laterMode}/${laterPcMode}: Index expands only its pinned carrier`);
    assert.equal(starts.length,embedded?0:1,'restore requests the pinned carrier');
    if (!embedded) assert.equal(starts[0].parameters.toplevelId,7,'child id cannot replace the root');
    assert.equal(wm.getDesktopRootId(),7,'restore preserves root');
    assert.equal(processes.length,1,'restore preserves the game process');
  }
  for (const session of [desktop,child]) {
    starts.length=0;index.launcherVisible=true;
    await service.resume(session);
    checkCarrier();
  }
  // Deck/classic local cards share this launch path when the game is running.
  service.sessions.set('game',child);starts.length=0;index.launcherVisible=true;
  const restored=await service.launch({id:'game',available:true,launchTarget:{kind:models.LaunchKind.WINE_EXE,
    executable:'C:/game.exe',arguments:[],workingDirectory:''}},settings,'/games');
  assert.equal(restored,child);checkCarrier();
  assert.ok(nativeCalls.some(([action,id])=>action==='raise'&&id===9),'game still receives focus');
  return {wm,service,context,starts,desktop,child,values};
}

(async()=>{
  for(const args of [
    ['tablet','false','floating'],['phone','false','floating'],
    ['2in1','true','floating'],['pc','unknown','floating'],
    ['pc','unknown','fullscreen'],['2in1','true','fullscreen'],
    ['tablet','false','floating','floating','true'],
    ['tablet','true','floating','floating','false'],
    ['pc','unknown','fullscreen','floating'],['tablet','false','floating','fullscreen']
  ]) await fixture(...args);
  console.log('Desktop restore: closed carrier, existing game, PC/Edge/phone/tablet, pinned settings/mode PASS');

  const f=await fixture('pc','unknown','floating');
  f.wm.markDesktopAbilityForeground(7);f.starts.length=0;
  f.wm.openDesktopWindow();
  assert.equal(f.starts.length,0,'initial ensure does not reopen a live carrier');
  await Promise.all([f.service.resume(f.child),f.service.resume(f.desktop)]);
  assert.equal(f.starts.length,1,'covered PC foreground carrier is raised once for explicit concurrent restores');
  assert.equal(f.wm.isDesktopAbilityForeground(7),true,'raising a foreground carrier preserves lifecycle confirmation');
  f.starts.length=0;
  f.wm.markDesktopAbilityBackground(7);
  let finish;
  f.context.startAbility=want=>{f.starts.push(want);return new Promise(resolve=>{finish=resolve;});};
  const a=f.service.resume(f.child),b=f.service.resume(f.desktop),c=f.wm.resumeDesktopHost();
  assert.equal(f.starts.length,1,'concurrent background restore shares one OS request');
  assert.equal(f.starts[0].parameters.toplevelId,7);finish();await Promise.all([a,b,c]);
  f.wm.markDesktopAbilityStopped(7);f.starts.length=0;
  f.context.startAbility=async want=>{f.starts.push(want);throw Error('OS rejected');};
  await assert.rejects(f.service.resume(f.child),/OS rejected/);
  assert.equal(f.wm.isDesktopAbilityRunning(7),false);
  assert.equal(f.values.get('desktopWindowActive'),false);
  f.context.startAbility=async want=>{f.starts.push(want);};
  await f.service.resume(f.child);
  assert.equal(f.starts.length,2,'failed restore can be retried');
  f.wm.markDesktopAbilityStopped(7);
  f.context.startAbility=()=>{throw Error('sync rejection');};
  await assert.rejects(f.wm.resumeDesktopHost(),/sync rejection/);
  assert.equal(f.wm.isDesktopAbilityRunning(7),false);

  // Foreground lifecycle confirmation releases a slow OS request for this root.
  let finishSlow;
  f.context.startAbility=()=>new Promise(resolve=>{finishSlow=resolve;});
  const slow=f.wm.resumeDesktopHost();
  f.wm.markDesktopAbilityForeground(7);f.wm.markDesktopAbilityBackground(7);
  f.starts.length=0;f.context.startAbility=async want=>{f.starts.push(want);};
  await f.wm.resumeDesktopHost();
  assert.equal(f.starts.length,1,'a completed foreground lifecycle cannot block a later restore');
  finishSlow();await slow;
  f.wm.markDesktopAbilityStopped(7);

  // A failed request from an old root must not clear a new carrier's state.
  let failOld;
  f.context.startAbility=()=>new Promise((resolve,reject)=>{failOld=reject;});
  const old=f.wm.resumeDesktopHost();
  const oldFailure=assert.rejects(old,/old root failure/);
  f.wm.setPresentationMode(models.WinePresentationMode.DESKTOP);
  f.context.startAbility=async()=>{};
  f.wm.onToplevelEvent(8,'desktop_root','{}');
  await f.wm.desktopAbilityRequest;
  f.wm.markDesktopAbilityForeground(8);
  failOld(Error('old root failure'));await oldFailure;
  assert.equal(f.wm.isDesktopAbilityRunning(8),true);
  assert.equal(f.wm.isDesktopAbilityForeground(8),true);
  f.wm.setPresentationMode(models.WinePresentationMode.MANAGED_WINDOWS);
  f.starts.length=0;await f.wm.resumeDesktopHost();
  assert.equal(f.starts.length,0,'managed mode cannot resurrect a desktop carrier');
  console.log('Desktop restore: foreground/background, request sharing, sync/async retry and stale-root isolation PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
