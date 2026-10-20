const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map();
const root = '/storage/test/SteamLinuxARM64';
let now = 100000;
function load(relative, deps, globals={}) {
  const out = {};
  const code = ts.transpileModule(fs.readFileSync(path.join(__dirname, '../entry/src/main/ets', relative), 'utf8'),
    {compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}}).outputText;
  vm.runInNewContext(code, {exports: out, Date: {now: () => now}, Set, Map, ...globals, require(id) {assert.ok(id in deps,id);return deps[id];}});
  return out;
}
const models = load('model/LinuxSteamModels.ets', {});
const {deckLayout} = load('model/DeckLayout.ets', {});
for (const [width,height] of [[320,640],[360,800],[412,915],[600,960],[800,360],[1280,800],[1024,600]]) {
  const l=deckLayout(width,height);
  assert.ok(l.contentWidth>0 && l.contentHeight>0);
  assert.equal(l.portrait,width<600 && height>width);
  const cardWidth=l.coverHeight/1.5;
  assert.ok(Math.abs(cardWidth*l.columns+(l.columns-1)*l.gap+2*l.inset-l.contentWidth)<0.001,
    `cards fit ${width}x${height} without overflow or distortion`);
  if(l.portrait) {assert.equal(l.columns,2);assert.equal(l.railWidth,0);assert.equal(l.contentHeight+72,height);}
}
assert.ok(deckLayout(0,NaN).contentHeight>0,'pre-layout area is bounded');
const {parseLinuxSteamWindowPreferences:parsePlacement} = load('model/LinuxSteamWindowPreferences.ets', {});
assert.equal(parsePlacement('1 0 960 600\n').width,960);
assert.equal(parsePlacement('1 1 1024 640\n').mode,1);
for(const invalid of ['1 2 1024 640','1 0 800 600 extra','2 0 800 600','1 0 4096 4096','1 0 10 20','x'.repeat(97)])
  assert.equal(parsePlacement(invalid),null,'invalid placement cannot affect launch');
console.log('Deck responsive geometry: phone portrait/landscape, split window, tablet, 2:3 art and bounded placement PASS');
const vdf = load('steam/SteamLocalRules.ets', {});
const stat = file => ({ isFile: () => files.has(file), isDirectory: () => file.endsWith('/common/Pal'), isSymbolicLink: () => false, size: (files.get(file)||'').length });
const library = load('service/LinuxSteamLibraryService.ets', {
  '@kit.CoreFileKit': {fileIo: {lstatSync:stat,statSync:stat,listFileSync: () => ['appmanifest_3590.acf','appmanifest_3590.acf','appmanifest_123.acf','appmanifest_5.acf']}},
  '../model/LinuxSteamModels':models,'../steam/SteamLocalRules':vdf,
  './ConfigurationFiles': {ConfigurationFiles: {text:x=>x,read(file) {if(!files.has(file))throw Error('missing');return files.get(file);}}},
  './ManagedSmokePayloadService': {readSmokeText: file => files.get(file)||''}
});
const manifest = (id,flags=4,dir='Pal') => `"AppState" { "appid" "${id}" "name" "仙剑奇侠传" "installdir" "${dir}" "StateFlags" "${flags}" "LastPlayed" "12345" }`;
const parse = library.linuxSteamGameFromManifest;
assert.equal(parse(manifest('3590'),'3590',root+'/library').name,'仙剑奇侠传');
assert.equal(parse(manifest('3590',6),'3590',root+'/library').updating,true);
for(const id of ['0','-1','4294967296','3590;rm','../3590'])assert.equal(parse(manifest(id),id,root),null);
assert.equal(parse(manifest('3590'),'123',root),null);
assert.equal(parse(manifest('228980'),'228980',root),null,'shared redistributables are not games');
assert.equal(parse(manifest('3590',2),'3590',root),null);
for(const dir of ['..','../Pal','a/b','a\\b','x:y'])assert.equal(parse(manifest('3590',4,dir),'3590',root),null);
files.set('/data/storage/el2/base/files/steamohos-native/external-root.path',root);
files.set(root+'/library/steamapps/appmanifest_3590.acf',manifest('3590'));
files.set(root+'/library/steamapps/appmanifest_123.acf',manifest('123',2));
files.set(root+'/library/steamapps/appmanifest_5.acf','incomplete {');
const service=library.LinuxSteamLibraryService.getInstance();
assert.equal(service.scan().length,1,'dedupe and incomplete/update-only manifests');
assert.equal(service.scan()[0].appId,'3590');
files.delete(root+'/library/steamapps/appmanifest_3590.acf');
assert.equal(service.scan().length,1,'bounded cache');now+=5001;assert.equal(service.scan().length,0,'uninstall refresh');
files.set('/data/storage/el2/base/files/steamohos-native/external-root.path','/storage/WindowsSteam');
assert.equal(service.scan(true).length,0,'Windows Steam cannot become Linux catalog');
console.log('Deck ArkTS library: Chinese names, identity, flags, boundaries, corrupt concurrent writes, dedupe, cache, uninstall and Windows isolation PASS');

function pageRecoveryTests() {
  const crash=n=>`[35137:35137:1020/101405.${n}:ERROR:bad_message.cc(29)] Terminating renderer for bad IPC message, reason 213\n`;
  let data=Buffer.from(crash(100001)),inode=1,closed=0,link=false,maxRead=0;
  const stat=()=>({size:data.length,ino:inode,isFile:()=>true,isSymbolicLink:()=>link});
  const {LinuxSteamPageRecovery}=load('service/LinuxSteamPageRecovery.ets',{
    '@kit.CoreFileKit':{fileIo:{OpenMode:{READ_ONLY:0,NOFOLLOW:1},lstatSync:stat,statSync:stat,
      openSync:()=>({fd:7}),closeSync(){closed++;},readSync(fd,buffer,{offset,length}){
        maxRead=Math.max(maxRead,length);return data.copy(Buffer.from(buffer),0,offset,offset+length);
      }}}
  });
  const observer=new LinuxSteamPageRecovery();observer.start('/cef_log.txt');
  observer.poll();assert.equal(observer.needed(),false,'historical failures never trigger navigation');
  const append=text=>{data=Buffer.concat([data,Buffer.from(text)]);now+=1001;observer.poll();};
  append('other errors including reason 213\n');assert.equal(observer.needed(),false,'only the verified Chromium failure qualifies');
  const split=crash(100002);append(split.slice(0,-8));assert.equal(observer.needed(),false);
  append(split.slice(-8));assert.equal(observer.take(),1,'split records are parsed once');
  append(split);assert.equal(observer.needed(),false,'duplicate log lines cannot loop recovery');
  append(crash(100003));assert.equal(observer.needed(),false,'fresh failures respect cooldown');
  now+=30001;assert.equal(observer.take(),2);
  append(crash(100004));now+=30001;assert.equal(observer.needed(),false,'recovery is bounded per browser helper');
  append(crash(100005).replace('[35137:', '[40000:'));
  assert.equal(observer.take(),3,'a new helper after another mode switch gets its own bounded recovery');
  observer.start('/cef_log.txt');inode++;data=Buffer.from(crash(100005));now+=1001;observer.poll();
  assert.equal(observer.take(),1,'log replacement is followed without replaying old history');
  data=Buffer.from(crash(100006)); // same inode, explicitly truncate then append
  data=Buffer.from('');now+=1001;observer.poll();append(crash(100006));now+=30001;
  assert.equal(observer.take(),2,'truncated logs are readable');
  observer.start('/cef_log.txt');link=true;append(crash(100007));assert.equal(observer.needed(),false,'symlinks are ignored');
  link=false;observer.start('/cef_log.txt');append('x'.repeat(90000)+'\n'+crash(100008));
  assert.equal(observer.take(),1);assert.ok(maxRead<=65536,'per-poll I/O is bounded');assert.ok(closed>0);
  observer.stop();append(crash(100009));assert.equal(observer.needed(),false,'stopped sessions cannot navigate');
  console.log('CEF recovery: historical logs, exact failure, split/duplicate records, rotation/truncation, cooldown, retry limit, symlinks, bounded I/O and stop PASS');
}
pageRecoveryTests();

async function sessionTests() {
  const singleton=x=>({getInstance:()=>x});
  let starts=0, launches=0, stopped=0, endNative;
  const commands=[];
  const base='/data/storage/el2/base/files/steamohos-native';
  const nativePromise=new Promise(resolve=>{endNative=resolve;});
  const config={read(file){if(!files.has(file))throw Error('missing');return files.get(file);},text:x=>x,json(file,text){
    files.set(file,text);
    if(file.endsWith('/deck-command.txt')) {
      const [version,generation,id,action,appId]=text.trim().split(' ');
      commands.push({generation,id,action,appId});
      files.set(base+'/client-tmp/deck-command-ack.json',JSON.stringify({version:1,generation,requestId:Number(id),status:'dispatched',exitCode:0}));
    }
  }};
  let foreground=true,recoverPending=false,recoverAttempts=0;
  class Host {async restore(){return true;} async start() {launches++;} stop(){stopped++;} isForeground(){return foreground;}}
  class Recovery {start(){}stop(){}poll(){}needed(){return recoverPending;}take(){recoverPending=false;return ++recoverAttempts;}}
  class Games {start(){}stop(){}}
  const nativeStart=()=>{starts++;const generation=files.get(base+'/session-generation.txt').trim();
    files.set(base+'/client-tmp/deck-session-state.json',JSON.stringify({version:1,generation,ready:true,closed:false,mode:0,updatedAt:now}));
    return nativePromise;
  };
  const api=load('service/LinuxSteamSessionService.ets',{
    '@kit.ArkUI':{promptAction:{showToast(){}}},
    '@kit.PerformanceAnalysisKit':{hilog:{info(){}}},
    '@ohos.util':{default:{generateRandomUUID:()=> 'abcd-9876'}},
    'libsmoke_napi.so':{default:{startLinuxSteamSession:nativeStart}},
    '../model/AppModels':{DisplayMode:{DESKTOP:1},WinePresentationMode:{DESKTOP:1}},
    '../model/LinuxSteamModels':models,
    './ConfigurationFiles':{ConfigurationFiles:config},
    './LinuxSteamInstallService':{LinuxSteamInstallService:singleton({ensureReady:async()=>{}})},
    './LinuxSteamLibraryService':{LinuxSteamLibraryService:singleton({externalRoot:()=>root})},
    './LinuxSteamGameLaunch':{LinuxSteamGameLaunch:Games},'./LinuxSteamWindowHost':{LinuxSteamWindowHost:Host},
    './LinuxSteamPageRecovery':{LinuxSteamPageRecovery:Recovery},
    './WineEngineService':{WineEngineService:singleton({ensureReady:async()=>true})},
    './HostWindowTasks':{HostWindowTasks:singleton({snapshot:()=>[{key:'steam-1',source:'steam-linux'}],activate:async()=>true,showLauncher:async()=>{}})},
    './GamepadManager':{GamepadManager:singleton({releaseAll(){}})},
    './ManagedSmokePayloadService':{ensureSmokeDirectory(){return true;},readSmokeText:file=>files.get(file)||''}
  },{setInterval(){return 1;},clearInterval(){},setTimeout(cb){now+=200;Promise.resolve().then(cb);}});
  const service=api.LinuxSteamSessionService.getInstance();service.initialize({});
  await Promise.all([service.open('library'),service.open('downloads'),service.open('launch','3590')]);
  assert.equal(starts,1,'concurrent callers share one product native session');
  assert.equal(launches,1,'one Wine host for that session');
  assert.deepEqual(commands.map(c=>c.action),['library','downloads','launch'],'commands serialized');
  assert.deepEqual(commands.map(c=>c.id),['1','2','3'],'monotonic requests');
  assert.equal(commands[2].appId,'3590');
  assert.equal(service.snapshot().state,'running');
  await assert.rejects(service.open('launch','../bad'),/AppID/);
  files.set(base+'/client-tmp/deck-session-state.json',JSON.stringify({version:1,generation:'old-generation',ready:true,closed:false,mode:2,updatedAt:now}));
  service.poll();assert.equal(service.snapshot().presentation,'desktop','ignore other-generation presentation');
  files.set(base+'/client-tmp/deck-session-state.json',JSON.stringify({version:1,generation:'abcd-9876',ready:true,closed:false,mode:2,updatedAt:now}));
  service.poll();assert.equal(service.snapshot().presentation,'bigPicture');
  files.set(base+'/client-tmp/deck-session-state.json',JSON.stringify({version:1,generation:'abcd-9876',ready:true,closed:false,mode:0,updatedAt:now-15000}));
  service.poll();assert.equal(service.snapshot().presentation,'bigPicture','stale file cannot reset presentation');
  recoverPending=true;service.recoverPage();assert.equal(service.recoveryQueued,false,'Big Picture is never redirected');
  files.set(base+'/client-tmp/deck-session-state.json',JSON.stringify({version:1,generation:'abcd-9876',ready:true,closed:false,mode:1,updatedAt:now}));
  foreground=false;service.poll();assert.equal(service.recoveryQueued,false,'a running game or launcher keeps focus');
  files.set(base+'/client-tmp/deck-command-ack.json',JSON.stringify({version:1,generation:'abcd-9876',requestId:8,status:'dispatched',exitCode:0}));
  foreground=true;service.poll();await service.tail;
  assert.equal(commands.at(-1).action,'library');assert.equal(recoverAttempts,1);assert.equal(starts,1,'renderer repair never restarts Steam');
  assert.equal(commands.at(-1).id,'9','fixed maintenance commands cannot leave recovery using a replayed request');
  recoverPending=true;service.recoverPage();
  await service.open('shutdown');assert.equal(service.snapshot().state,'stopping');
  assert.equal(recoverAttempts,1,'new user shutdown cancels pending repair');
  endNative('{"status":"completed"}');await service.worker;assert.equal(service.snapshot().state,'stopped');
  assert.equal(stopped,1,'host cleaned after real native worker exit');
  console.log('Deck ArkTS session: concurrent start, command ordering, AppID, fresh generation, mode ACK, stale state and normal shutdown PASS');
}
async function taskTests() {
  const {HostWindowTasks} = load('service/HostWindowTasks.ets', {});
  const tasks = HostWindowTasks.getInstance();
  tasks.onEvent(4,'created',JSON.stringify({clientPid:100,sessionId:'steam-host'}));
  tasks.onEvent(4,'title',JSON.stringify({title:'Steam (Linux ARM64)'}));
  const key=tasks.snapshot()[0].key;
  let complete;
  tasks.setActivator(async()=>new Promise(resolve=>{complete=resolve;}));
  const restore=tasks.activate(key);await Promise.resolve();
  tasks.markLauncherForeground();complete(true);
  assert.equal(await restore,false,'returning to Deck cancels an in-flight task switch');
  assert.equal(tasks.hasForegroundTask(),false);
  assert.equal(tasks.snapshot()[0].key,key,'returning home preserves window identity');
  tasks.setActivator(async()=>true);
  assert.equal(await tasks.activate(key),true);
  assert.equal(tasks.isLauncherForeground(),false);
  tasks.markLauncherForeground();
  tasks.onEvent(4,'destroyed','{}');
  assert.equal(tasks.snapshot().length,0);
  assert.equal(tasks.hasForegroundTask(),false);
  console.log('Deck tasks: launcher focus, cancelled in-flight restore, stable window identities and explicit restore PASS');
}
Promise.all([sessionTests(),taskTests()]).catch(error=>{console.error(error);process.exitCode=1;});
