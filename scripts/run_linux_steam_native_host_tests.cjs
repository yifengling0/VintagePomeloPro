const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),ts=require(process.argv[2]);
let current=[{key:'client-1',source:'steam-linux',clientPid:11,toplevelId:1,minimized:false},
 {key:'unknown',source:'native',clientPid:22,toplevelId:2,minimized:false}];
let refreshes=0,navigation=0,closed=0,focused=1,launcher=false;const managed=[],activated=[];
const singleton=x=>({getInstance:()=>x});const tasks={
 refreshLinuxSteamOwnership(fn){refreshes++;assert.equal(fn(11),true);},snapshot:()=>current,
 isLauncherForeground:()=>launcher,isSteamGameForeground:()=>false,
 activate:async key=>{activated.push(key);return true;},activateSteamLaunchPrompt:async key=>{activated.push(key);return true;}};
const deps={
 '@kit.AbilityKit':{},'libentry.so':{default:{getKeyboardFocusedToplevel:()=>focused}},
 'libsmoke_napi.so':{default:{isLinuxSteamProcess:pid=>pid===11}},
 './HostWindowTasks':{HostWindowTasks:singleton(tasks)},
 './WineWindowManager':{WineWindowManager:singleton({manageExternalToplevel:key=>managed.push(key)})},
 './AppSettingsStore':{AppSettingsStore:singleton({getGlobalSettings:()=>({desktopInputProfileId:'user'})})},
 './InputProfileService':{InputProfileService:singleton({getProfile:id=>({id,gamepadMappings:[]})})},
 './GamepadManager':{GamepadManager:singleton({releaseAll(){},setHostNavigation(){},setOutputMode(){},setButtonMappings(){navigation++;}})},
 './InputOverlayWindowCoordinator':{InputOverlayWindowCoordinator:singleton({getSnapshot:()=>({}),update(){}})}
};
const api={};let tick;
const code=ts.transpileModule(fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxSteamNativeWindowHost.ets'),'utf8'),
 {compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
vm.runInNewContext(code,{exports:api,setInterval:f=>{tick=f;return 1;},clearInterval:()=>closed++,require:id=>{assert.ok(id in deps,id);return deps[id];}});
(async()=>{
 const host=new api.LinuxSteamNativeWindowHost();await host.start({},'', '');
 assert.deepEqual(managed,['client-1']);assert.equal(host.isForeground(),true);assert.equal(await host.restore(),true);
 assert.deepEqual(activated,['client-1']);
 launcher=true;tick();assert.equal(host.isForeground(),false);assert.ok(navigation>=2,'foreground and launcher restore user input profile');
 current=[{key:'reused-1',source:'steam-linux',clientPid:22,toplevelId:1}];tick();assert.equal(managed.includes('reused-1'),false,'recycled window with foreign PID is never claimed');
 host.stop();const before=refreshes;tick();assert.equal(refreshes,before);assert.equal(closed,1);assert.equal(host.hasExited(),true);
 console.log('Native Steam carrier: authenticated window generation, foreground/navigation, explicit restore, stop and no Wine initialization PASS');
})().catch(e=>{console.error(e);process.exitCode=1;});
