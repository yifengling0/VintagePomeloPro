const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '../entry/src/main/ets');
const values = new Map();
const AppStorage = {get:key=>values.get(key), setOrCreate:(key,value)=>values.set(key,value)};
const hilog = {info(){},warn(){},error(){}};
const models = {DesktopWindowMode:{FLOATING:'floating',FULLSCREEN:'fullscreen',SAFE_AREA:'safe_area'},
  ScreenOrientationSetting:{PORTRAIT:'portrait',LANDSCAPE:'landscape',SENSOR_LANDSCAPE:'sensor-landscape'},
  WinePresentationMode:{DESKTOP:'desktop',MANAGED_WINDOWS:'managed-windows'}};
function load(name,deps,globals={}) {
  const exports={};
  const code=ts.transpileModule(fs.readFileSync(path.join(root,name),'utf8'),{compilerOptions:{
    target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
  vm.runInNewContext(code,{exports,AppStorage,Set,Map,setTimeout,clearTimeout,...globals,
    require(id){if(id in deps)return deps[id];throw Error('missing mock '+id);}});
  return exports;
}
const status={FULL_SCREEN:1,MAXIMIZE:2,FLOATING:4};
const windowApi={WindowStatusType:status,MaximizePresentation:{ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER:2},Orientation:{},RectChangeReason:{DRAG:4,DRAG_START:5,DRAG_END:6}};
let device={deviceType:'tablet'},setting='unknown',observer=null,reads=0,registered=0,unregistered=0;
const ctx={};let failRead=false;
const policy=load('common/DeviceCapabilityPolicy.ets',{
  '../model/ValueAddedModels':{},'@ohos.deviceInfo':{default:device},
  '@kit.BasicServicesKit':{settings:{domainName:{USER_PROPERTY:'user'},
    getValueSync(context,key,fallback,domain){assert.equal(key,'settings.sceneboard.ispcmode');assert.equal(domain,'user');reads++;if(failRead)throw Error('temporarily unavailable');return setting;},
    registerKeyObserver(context,key,domain,callback){registered++;observer=callback;return true;},
    unregisterKeyObserver(){unregistered++;return true;}}},
  '@kit.ArkUI':{window:windowApi},'../service/LogService':{hilog},'../model/AppModels':models
}).DeviceCapabilityPolicy;
policy.startPcModeMonitoring(ctx);
assert.equal(reads,2,'read before and after observer registration');
assert.equal(policy.usesManagedWineWindows(),false);
let notifications=0;const listener=()=>notifications++;
policy.addPcModeListener(listener);
for(const [type,mode,expected] of [
  ['tablet','true',true],['tablet','false',false],['tablet','unknown',false],
  ['2in1','unknown',true],['2in1','false',false],['2in1','true',true],
  ['phone','true',false],['tv','true',false],['pc','false',true]]) {
  device.deviceType=type;setting=mode;policy.refreshPcMode(ctx);
  assert.equal(policy.usesManagedWineWindows(),expected,`${type}/${mode}`);
}
device.deviceType='tablet';setting='true';policy.refreshPcMode(ctx);
assert.equal(policy.usesForkNativeChildProcess(),false,'PC mode never changes the NCP ABI policy');
policy.removePcModeListener(listener);const count=notifications;
setting='false';observer();assert.equal(notifications,count);
policy.stopPcModeMonitoring(ctx);assert.equal(unregistered,1);
policy.startPcModeMonitoring(ctx);assert.equal(registered,2);
setting='true';policy.refreshPcMode(ctx);failRead=true;policy.refreshPcMode(ctx);
assert.equal(policy.usesManagedWineWindows(),true,'transient failure preserves the last known workspace');failRead=false;
const sessionCtx={};policy.startPcModeMonitoring(sessionCtx);policy.stopPcModeMonitoring(ctx);
assert.equal(registered,3,'closing launcher rebinds the observer to the remaining session');
setting='false';observer();assert.equal(policy.usesManagedWineWindows(),false);
policy.stopPcModeMonitoring(sessionCtx);
console.log('Edge tablet/PC switch, legacy fallback, non-latching policy and observer lifecycle PASS');

async function fullscreenTests(){
  let mode='fullscreen',requestCount=0,finish;
  const wm={isDesktopMode:true,enterImmersiveFullscreen:()=>{requestCount++;return new Promise(resolve=>finish=resolve);}};
  const DWA=load('entryability/DesktopAbility.ets',{
    '@kit.AbilityKit':{UIAbility:class{}},'@kit.ArkUI':{window:windowApi},'libentry.so':{},
    '../service/LogService':{hilog},'../service/WineWindowManager':{WineWindowManager:{getInstance:()=>wm}},
    '../service/WineImeCoordinator':{},'../service/GamepadManager':{},'../service/InputRouter':{},'../service/InputDispatcher':{},
    '../service/InputOverlayWindowCoordinator':{},'../service/AppSettingsStore':{AppSettingsStore:{getInstance:()=>({getGlobalSettings:()=>({desktopWindowMode:mode})})}},
    '../model/AppModels':models,'../service/WineEngineService':{},'../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:policy}
  }).default;
  const ability=new DWA(),events={};
  const win={on:(name,cb)=>events[name]=cb,setWindowTitle(){},setWindowLayoutFullScreen:async()=>{},
    setSpecificSystemBarEnabled:async()=>{},setWindowBackgroundColor:async()=>{}};
  ability.attachVirtualDesktopStatus(win);
  values.set('vdImmersive',false);
  events.windowStatusChange(status.MAXIMIZE);
  events.windowStatusChange(status.MAXIMIZE);
  assert.equal(requestCount,1,'MAXIMIZE feedback cannot issue duplicate fullscreen requests');
  assert.equal(values.get('vdImmersive'),false,'accepting a request is not FULL_SCREEN confirmation');
  finish(true);await new Promise(setImmediate);
  assert.equal(values.get('vdImmersive'),false);
  events.windowStatusChange(status.FULL_SCREEN);assert.equal(values.get('vdImmersive'),true);
  events.windowStatusChange(status.FLOATING);assert.equal(values.get('vdImmersive'),false);
  events.windowStatusChange(status.MAXIMIZE);finish(false);await new Promise(setImmediate);
  assert.equal(ability.immersiveMaximized,false,'failure permits a future retry');
  values.set('vdRestoring',true);events.windowStatusChange(status.MAXIMIZE);
  assert.equal(requestCount,2,'EXIT_IMMERSIVE intermediate MAXIMIZE cannot re-enter fullscreen');
  values.set('vdRestoring',false);
  setting='true';policy.refreshPcMode(ctx);mode='floating';
  await ability.applyDesktopWindowMode(win);assert.equal(requestCount,2,'floating preference remains windowed');
  assert.equal(ability.lastAppliedWindowMode,'pc-floating');
  mode='fullscreen';const applied=ability.applyDesktopWindowMode(win);await new Promise(setImmediate);await Promise.resolve();
  finish(true);await applied;
  assert.equal(ability.lastAppliedWindowMode,'pc-fullscreen');
  console.log('Fullscreen system confirmation, duplicate requests, failure, restore and windowed preference PASS');
}

async function sceneIdentityTests(){
  const resolvers=new Map(),identities=[],ready=[];
  class LocalStorage {constructor(initial){this.values=new Map(Object.entries(initial||{}));}get(k){return this.values.get(k);}setOrCreate(k,v){this.values.set(k,v);}}
  const FWM=load('service/FusionWindowManager.ets',{
    '@kit.ArkUI':{window:windowApi,display:{getDefaultDisplaySync:()=>({width:3120,height:2080})}},
    './LogService':{hilog},'libentry.so':{default:{cancelPendingToplevel(){}}}
  },{LocalStorage}).FusionWindowManager.getInstance();
  const stage={createSubWindowWithOptions:name=>new Promise(resolve=>resolvers.set(name,resolve)),
    getMainWindowSync:()=>({getWindowProperties:()=>({id:1}),setWindowSystemBarEnable:async()=>{}})};
  FWM.initIfNeeded(stage,()=>2);
  for(const id of [10,20,30])FWM.ensure(id,640,480,(id,win)=>ready.push([id,win]));
  function window(id){return {async loadContent(page,storage){identities.push([id,storage.get('wineToplevelId')]);},
    getWindowProperties:()=>({id}),setWindowCornerRadius(){},setWindowBackgroundColor(){},enableDrag:async()=>{},
    setWindowKeepScreenOn(){},resize:async()=>{},moveWindowToGlobal:async()=>{},showWindow:async()=>{}};}
  for(const id of [30,10,20]){resolvers.get('wine_fusion_'+id)(window(id));await new Promise(setImmediate);}
  assert.deepEqual(identities,[[30,30],[10,10],[20,20]],'reverse completion cannot bind a different renderer');
  assert.deepEqual(ready.map(x=>x[0]),[30,10,20]);
  console.log('Concurrent scene creation with out-of-order completion keeps renderer IDs isolated PASS');
}

async function carrierTests(){
  let settingMode='floating';const starts=[],resizes=[];
  const wm=load('service/WineWindowManager.ets',{
    '@kit.AbilityKit':{},'@kit.ArkUI':{window:windowApi},'./LogService':{hilog},
    '@kit.BasicServicesKit':{},'@kit.InputKit':{},'libentry.so':{default:{setDesktopMode(){},notifyToplevelResize:(...x)=>resizes.push(x),notifyToplevelResizeEnd:id=>resizes.push([id,'end'])}},'libsmoke_napi.so':{default:{}},
    './HostWindowTasks':{HostWindowTasks:{getInstance:()=>({reset(){},onEvent(){}})}},
    './InputDispatcher':{},'../model/AppModels':models,'./PopupWindowManager':{},'./ModalWindowManager':{},'./FusionWindowManager':{},
    './AppSettingsStore':{AppSettingsStore:{getInstance:()=>({usesManagedWineWindows:()=>policy.usesManagedWineWindows(),getGlobalSettings:()=>({desktopWindowMode:settingMode})})}},
    './DesktopModeService':{},'../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:policy}
  }).WineWindowManager.getInstance();
  wm.uiAbilityContext={startAbility:async want=>{starts.push(want);}};
  wm.startFusionSubWindow=(id)=>starts.push({subwindow:id});
  device.deviceType='tablet';setting='false';policy.refreshPcMode(ctx);
  wm.setPresentationMode('managed-windows');wm.startWineWindowAbility(10,'{}');
  assert.equal(starts[0].subwindow,10);
  setting='true';policy.refreshPcMode(ctx);wm.startWineWindowAbility(20,'{}');
  assert.equal(starts[1].subwindow,20,'live tablet session cannot change carrier halfway through');
  wm.setPresentationMode('managed-windows');wm.startWineWindowAbility(30,'{}');await new Promise(setImmediate);
  assert.equal(starts[2].abilityName,'WineWindowAbility','new Edge PC session uses an independent Ability');
  wm.startWineWindowAbility(30,'{}');await new Promise(setImmediate);
  assert.equal(starts.length,3,'AMS acceptance does not release the carrier reservation before WindowStage');
  wm.registerWindow(30,{});assert.equal(wm.pendingStarts.has(30),false);
  wm.startWineWindowAbility(30,'{}');await new Promise(setImmediate);
  assert.equal(starts.length,3,'registered windows cannot be launched again by ownership polling');
  setting='false';policy.refreshPcMode(ctx);assert.equal(wm.fusionUsesSubWindow(),false);
  wm.setPresentationMode('desktop');wm.onToplevelEvent(50,'desktop_root','{}');
  assert.equal(values.get('desktopEmbeddedHost'),true);assert.equal(starts.length,3);
  setting='true';policy.refreshPcMode(ctx);wm.onToplevelEvent(50,'desktop_root','{}');
  assert.equal(values.get('desktopEmbeddedHost'),true,'mode switch cannot steal an embedded renderer');
  wm.setPresentationMode('desktop');wm.onToplevelEvent(60,'desktop_root','{}');await new Promise(setImmediate);
  assert.equal(values.get('desktopEmbeddedHost'),false);assert.equal(starts[3].abilityName,'DesktopAbility');
  setting='false';policy.refreshPcMode(ctx);wm.onToplevelEvent(60,'desktop_root','{}');
  assert.equal(values.get('desktopEmbeddedHost'),false,'dedicated root must not appear in the launcher too');
  let n=0;assert.equal(await wm.enterImmersiveFullscreen({maximize:async mode=>{n++;if(mode!==undefined)throw Error('unsupported');}}),true);
  assert.equal(n,2);assert.equal(await wm.enterImmersiveFullscreen({maximize:async()=>{throw Error('rejected');}}),false);
  console.log('Session carrier pinning, desktop single ownership and fullscreen API fallbacks PASS');
  const windowSizes=[];
  wm.isDesktopMode=false;wm.displayScale=2;wm.scaleBoost=1;wm.fusionUsesSubWindow=()=>false;
  wm.registerWindow(77,{resize:(...x)=>windowSizes.push(x)});
  const rect=(width,height)=>({left:100,top:100,width,height});
  wm.onHostWindowRectChange(77,{reason:3,rect:rect(1280,960)});
  assert.equal(resizes.length,0,'moving cannot start a resize session');
  wm.onHostWindowRectChange(77,{reason:5,rect:rect(1280,960)});
  wm.onHostWindowRectChange(77,{reason:4,rect:rect(1440,1000)});
  assert.deepEqual(resizes.at(-1),[77,720,500,true]);
  wm.applyEvent(77,'resize',JSON.stringify({w:680,h:490}));
  assert.equal(windowSizes.length,0,'old Wine frames cannot pull the user drag backward');
  wm.onHostWindowRectChange(77,{reason:6,rect:rect(1440,1000)});
  assert.equal(wm.isHostWindowResizing(77),true);
  wm.applyEvent(77,'resize',JSON.stringify({w:720,h:500}));
  assert.deepEqual(resizes.at(-1),[77,'end']);assert.equal(wm.isHostWindowResizing(77),false);
  wm.applyEvent(77,'resize',JSON.stringify({w:800,h:600}));
  assert.deepEqual(windowSizes.at(-1),[1600,1200],'programmatic sizes still apply outside user dragging');
  wm.onHostWindowRectChange(77,{reason:5,rect:rect(1280,960)});
  wm.onHostWindowRectChange(77,{reason:6,rect:rect(1280,960)});
  await new Promise(resolve=>setTimeout(resolve,550));
  assert.equal(wm.isHostWindowResizing(77),false,'unresponsive client cannot retain resize state forever');
  console.log('Managed border resize state, Wine acknowledgment, feedback isolation and bounded teardown PASS');

}

fullscreenTests().then(sceneIdentityTests).then(carrierTests).catch(error=>{console.error(error);process.exitCode=1;});
