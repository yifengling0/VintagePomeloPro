#!/usr/bin/env node
const assert=require('node:assert/strict'), fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto'),os=require('node:os');
const ts=require(process.argv[2]), source=path.resolve(__dirname,'../entry/src/main/ets');
let count=0;
function fixture({library,filesDir,persistent,deny,failFlush,cloudApi}={}) {
 const temp=filesDir?path.dirname(filesDir):fs.mkdtempSync(path.join(os.tmpdir(),'pomelo-config-'));
 const context={filesDir:filesDir||path.join(temp,'sandbox'),cacheDir:path.join(temp,'cache')};fs.mkdirSync(context.filesDir,{recursive:true});fs.mkdirSync(context.cacheDir,{recursive:true});
 context.getApplicationContext=()=>context;
 library=library||path.join(temp,'library');fs.mkdirSync(library,{recursive:true});const stores=persistent||new Map(),cache=new Map(),timers=new Map();let nextTimer=0;
 const prefs={getPreferences:async(_,name)=>getStore(name),getPreferencesSync:(_,options)=>getStore(options.name)};
 function getStore(name){if(stores.has(name))return stores.get(name);const values=new Map(),listeners=[],dirty=new Set();const store={values,
   getSync:(k,d)=>values.has(k)?values.get(k):d,hasSync:k=>values.has(k),put:async(k,v)=>{values.set(k,v);dirty.add(k);},putSync:(k,v)=>{values.set(k,v);dirty.add(k);},delete:async k=>{values.delete(k);dirty.add(k);},
   flush:async()=>{store.flushes=(store.flushes||0)+1;if(await failFlush?.(name))throw Error('INTERRUPTED');for(const k of dirty)for(const listener of listeners)listener(k);dirty.clear();},on:(_,listener)=>listeners.push(listener)};stores.set(name,store);return store;}
 const io={OpenMode:{READ_ONLY:1,WRITE_ONLY:2,CREATE:4,TRUNC:8,NOFOLLOW:16},accessSync:p=>fs.existsSync(p),mkdirSync:(p,recur)=>fs.mkdirSync(p,{recursive:!!recur}),lstatSync:fs.lstatSync,statSync:p=>{const s=typeof p==='number'?fs.fstatSync(p):fs.statSync(p);s.mtime=s.mtimeMs;return s;},
  openSync:(p,mode)=>{if(deny?.(p)&&mode&2)throw Error('READ_ONLY');return {fd:fs.openSync(p,mode&2?((mode&8)?'w':(mode&4)?'w':'r+'):'r'),path:p};},closeSync:f=>fs.closeSync(typeof f==='number'?f:f.fd),
  readSync:(fd,b,opts)=>fs.readSync(fd,Buffer.from(b),0,opts?.length??b.byteLength,null),writeSync:(fd,b)=>fs.writeSync(fd,typeof b==='string'?b:Buffer.from(b)),fsyncSync:fs.fsyncSync,
  renameSync:(a,b)=>{if(deny?.(b))throw Error('READ_ONLY');fs.renameSync(a,b);},unlinkSync:fs.unlinkSync,readTextSync:p=>fs.readFileSync(p,'utf8'),copyFile:async(a,b)=>{fs.writeFileSync(b,typeof a==='number'?fs.readFileSync(a):fs.readFileSync(a));},listFileSync:fs.readdirSync};
 const util={TextEncoder:class {encodeInto(s){return new Uint8Array(Buffer.from(s));}},TextDecoder:{create:()=>({decodeWithStream:b=>Buffer.from(b).toString('utf8')})},generateRandomUUID:crypto.randomUUID,
   Base64Helper:class {encodeToStringSync(b){return Buffer.from(b).toString('base64');}decodeSync(s){return new Uint8Array(Buffer.from(s,'base64'));}}};
 const externals={'@kit.AbilityKit':{},'@ohos.data.preferences':{default:prefs},'@kit.ArkData':{preferences:prefs},'@kit.CoreFileKit':{fileIo:io,fileUri:{FileUri:class{constructor(){this.path=library;}}}},
  '@ohos.bundle.bundleManager':{default:{BundleFlag:{GET_BUNDLE_INFO_WITH_APPLICATION:1},getBundleInfoForSelfSync:()=>({name:'test.pomelo',versionName:'1.4.8'})}},'@ohos.util':{default:util},
  '@ohos.net.http':{default:{RequestMethod:{GET:'GET',POST:'POST',PUT:'PUT'}}},
  '@ohos.security.cryptoFramework':{default:{createMd:()=>{const digest=crypto.createHash('sha256');return {update:async({data})=>{digest.update(data);},digest:async()=>({data:new Uint8Array(digest.digest())})};}}}};
 function load(file){file=path.resolve(file);if(cache.has(file))return cache.get(file);const exports={};cache.set(file,exports);
   const requireImpl=name=>{if(externals[name])return externals[name];if(name.endsWith('LogService'))return {hilog:{info(){},warn(){},error(){}}};
    if(cloudApi&&name.endsWith('ValueAddedService'))return {ValueAddedService:{getInstance:()=>cloudApi}};
    if(name.endsWith('DeviceCapabilityPolicy'))return {DeviceCapabilityPolicy:{isTabletDevice:()=>false,usesManagedWineWindows:()=>false}};
    if(name.startsWith('.'))return load(path.resolve(path.dirname(file),name+'.ets'));throw Error('Missing test dependency '+name);};
   const js=ts.transpileModule(fs.readFileSync(file,'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
   vm.runInNewContext(js,{exports,require:requireImpl,console,Uint8Array,ArrayBuffer,DataView,Set,Map,Date,JSON,Math,Number,AppStorage:{setOrCreate(){}},
    setTimeout:f=>{const id=++nextTimer;timers.set(id,f);return id;},clearTimeout:id=>timers.delete(id)},{filename:file});return exports;}
 const models=load(path.join(source,'model/AppModels.ets')),zip=load(path.join(source,'model/ConfigurationZip.ets'));
 const settings=load(path.join(source,'service/AppSettingsStore.ets')).AppSettingsStore.getInstance(),profiles=load(path.join(source,'service/InputProfileService.ets')).InputProfileService.getInstance();
 const portable=load(path.join(source,'service/PortableGameConfigurationService.ets')).PortableGameConfigurationService.getInstance(),repo=load(path.join(source,'service/UserConfigurationRepository.ets')).UserConfigurationRepository.getInstance();
 return {context,library,stores,settings,profiles,portable,repo,models,zip,load,timers,
  initialize:async()=>{await repo.initialize(context);await settings.initialize(context);await profiles.initialize(context);await portable.initialize(context);},
  dispose:()=>{if(!filesDir)fs.rmSync(temp,{recursive:true,force:true});},getStore};
}
async function test(name,fn){await fn();console.log('PASS '+name);count++;}
const same=(a,b)=>assert.equal(JSON.stringify(a),JSON.stringify(b));
async function cloudFixture(){
 const {Configurations}=await import('../server/value-added/src/configurations.mjs'),{MemoryStore}=await import('../server/value-added/test/memory-store.mjs');
 const store=new MemoryStore(),objects=new Map(),hash=b=>crypto.createHash('sha256').update(b).digest('hex'),calls=[];
 const service=new Configurations({store,gate(){},requireMembership:async()=>{}},{ready:true,
  put:async(k,b,h)=>{assert.equal(hash(b),h);objects.set(k,Buffer.from(b));},read:async(k,n)=>{const b=objects.get(k);assert.equal(b.length,n);return Buffer.from(b);},delete:async k=>objects.delete(k)});
 let owner='a'.repeat(64),hook=null;
 const api={getUserId:()=>owner,configurationRequest:async(route,method,raw)=>{
  const user=owner;calls.push({route,method,bytes:raw?.length||0});if(hook)await hook(route,method);
  const parts=route.split('/').filter(Boolean),body=raw?JSON.parse(raw):{};
  if(method==='GET'&&parts.length===3)return service.current(user,parts[2]);
  if(parts[2]==='uploads'&&parts.length===3)return service.begin(user,body);
  if(parts[4]==='lease')return service.lease(user,parts[3]);
  if(parts[4]==='abort')return service.abort(user,parts[3]);
  if(parts[4]==='finalize')return service.finalize(user,parts[3]);
  if(parts[4]==='chunks')return method==='GET'?service.read(user,parts[3],parts[5]):service.upload(user,parts[3],body);
  throw Error('UNKNOWN_ROUTE');
 }};
 return {store,objects,calls,service,api,owner:v=>owner=v,hook:f=>hook=f};
}
async function client(f,c,previousOwner=''){
 const service=f.load(path.join(source,'service/valueadded/ConfigurationCloudSyncService.ets')).ConfigurationCloudSyncService.getInstance();await service.initialize(f.context,previousOwner);return service;
}
async function editScale(f,value){const global=f.models.createDefaultGlobalSettings();global.outputScale=value;await f.settings.saveGlobalSettings(global);}
(async()=>{
 await test('complete game layout survives reinstall and a Chinese directory move',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'games','游戏 A');fs.mkdirSync(game,{recursive:true});const exe=path.join(game,'game.exe');fs.writeFileSync(exe,'executable');
  const p=f.models.createDefaultInputProfile();p.id='app-legacy';p.cursorSpeed=1.8;p.elements[0].x=0.42;
  await f.profiles.saveProfile(p);const value=f.models.createDefaultAppSettings('legacy');value.inputProfileId=p.id;value.arguments=['中文','-window'];value.box64Preset='conservative';value.executable=exe;value.workingDirectory=game;await f.settings.saveAppSettings(value);
  const id=await f.portable.discover(game,exe,'legacy');const side=JSON.parse(fs.readFileSync(path.join(game,'.pomelo/game.json')));assert.equal(side.entries[0].input.mode,'dedicated');p.id=side.entries[0].input.profile.id;same(side.entries[0].input.profile,p);
  const moved=path.join(f.library,'games','Moved 游戏');fs.renameSync(game,moved);
  const g=fixture({library:f.library,filesDir:path.join(path.dirname(f.context.filesDir),'reinstalled')});await g.initialize();const recovered=await g.portable.discover(moved,path.join(moved,'game.exe'),'path-new');assert.equal(recovered,id);
  const settings=g.settings.getAppSettings(id);same(settings.arguments,value.arguments);assert.equal(settings.executable,path.join(moved,'game.exe'));assert.equal(settings.box64Preset,'conservative');
  const restored=g.profiles.getProfile(settings.inputProfileId);p.id=restored.id;same(restored,p);f.dispose();
 });
 await test('shared binding stays shared and deleted profiles are not resurrected by sidecars',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'shared');fs.mkdirSync(game);const exe=path.join(game,'g.exe');fs.writeFileSync(exe,'g');const p=f.models.createDefaultInputProfile();p.id='shared';await f.profiles.saveProfile(p);
  const value=f.models.createDefaultAppSettings('old');value.inputProfileId='shared';await f.settings.saveAppSettings(value);const id=await f.portable.discover(game,exe,'old');
  const side=JSON.parse(fs.readFileSync(path.join(game,'.pomelo/game.json')));assert.equal(side.entries[0].input.mode,'shared');assert.equal(f.settings.getAppSettings(id).inputProfileId,'shared');
  await f.profiles.removeProfile('shared');await f.portable.flush();assert.equal(f.settings.getAppSettings(id).inputProfileId,'default');assert.ok(f.profiles.isDeleted('shared'));f.dispose();
 });
 await test('read-only sidecar uses complete fallback; a later writable refresh commits it',async()=>{
  let denied=true;const f=fixture({deny:p=>denied&&p.includes('/readonly/.pomelo/')});await f.initialize();const game=path.join(f.library,'readonly');fs.mkdirSync(game);const exe=path.join(game,'g.exe');fs.writeFileSync(exe,'g');
  const id=await f.portable.discover(game,exe,'old');let value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.arguments=['-custom'];await f.settings.saveAppSettings(value);
  assert.equal(fs.existsSync(path.join(game,'.pomelo/game.json')),false);const snapshot=await f.repo.snapshot();const manifest=JSON.parse(Buffer.from(snapshot[0].bytes));assert.ok(manifest.modules.find(m=>m.name==='library').entries.some(e=>e.store==='pomelo_game_config'&&e.value.includes('-custom')));
  denied=false;await f.portable.discover(game,exe,id);assert.ok(fs.existsSync(path.join(game,'.pomelo/game.json')));f.dispose();
 });
 await test('future or corrupt game formats are preserved byte for byte',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'future');fs.mkdirSync(path.join(game,'.pomelo'),{recursive:true});const exe=path.join(game,'g.exe');fs.writeFileSync(exe,'g');const file=path.join(game,'.pomelo/game.json');
  for(const text of ['{"formatVersion":99,"unknown":"keep"}','not-json']){fs.writeFileSync(file,text);assert.equal(await f.portable.discover(game,exe,'old'),'old');await f.settings.saveAppSettings(f.models.createDefaultAppSettings('old'));assert.equal(fs.readFileSync(file,'utf8'),text);}f.dispose();
 });
 await test('complete system settings, wallpaper bytes, fonts and profiles restore together',async()=>{
  const f=fixture();await f.initialize();await f.repo.start();const picture=path.join(f.library,'wallpaper.png');fs.writeFileSync(picture,Buffer.alloc(400,5));const wallpaper=await f.repo.importWallpaper(picture);
  const global=f.models.createDefaultGlobalSettings();global.pageBackgroundUri=wallpaper;global.usePageBackground=true;global.outputScale=77;global.gamepadDeadzone=0.23;global.virtualMouseConfig.sensitivity=1.4;await f.settings.saveGlobalSettings(global);
  const input=f.getStore('input_settings');await input.put('cursorSpeed',1.7);await input.put('padButtons','[{"id":"A","key":48}]');await input.flush();
  const fonts=path.join(f.context.filesDir,'.wine/drive_c/windows/fonts');fs.mkdirSync(fonts,{recursive:true});fs.writeFileSync(path.join(fonts,'Custom.ttf'),'font bytes');await f.repo.recordFonts(['Custom.ttf'],fonts);
  const members=await f.repo.snapshot(),archive=f.zip.encodeConfigurationZip(members);same(f.zip.decodeConfigurationZip(archive).map(m=>m.name),members.map(m=>m.name));assert.ok(members.length===3);
  const g=fixture({library:path.join(f.library,'newlibrary'),filesDir:path.join(path.dirname(f.context.filesDir),'newSandbox')});await g.initialize();await g.repo.restore(members,g.repo.revision());g.settings.reloadConfiguration();await g.profiles.reloadConfiguration();
  const restored=g.settings.getGlobalSettings();assert.equal(restored.outputScale,77);assert.equal(restored.gamepadDeadzone,0.23);assert.deepEqual(fs.readFileSync(restored.pageBackgroundUri),Buffer.alloc(400,5));
  assert.equal(g.getStore('input_settings').getSync('cursorSpeed',0),1.7);assert.equal(fs.readFileSync(path.join(g.context.filesDir,'.wine/drive_c/windows/fonts/Custom.ttf'),'utf8'),'font bytes');
  await g.repo.backupLocal();assert.ok(fs.existsSync(path.join(g.library,'.pomelo/app-settings-backup.zip')));f.dispose();
 });
 await test('fresh install restores Z backup before input and global consumers hydrate',async()=>{
  const f=fixture();await f.initialize();await f.repo.start();const settings=f.models.createDefaultGlobalSettings();settings.outputScale=65;await f.settings.saveGlobalSettings(settings);await f.repo.backupLocal();
  const g=fixture({library:f.library,filesDir:path.join(path.dirname(f.context.filesDir),'offlineReinstall')});await g.initialize();assert.equal(g.settings.getGlobalSettings().outputScale,65);assert.equal(g.repo.hasLocalUserData(),true);f.dispose();
 });
 await test('future backup and changed local revision cannot be overwritten by a restore',async()=>{
  const f=fixture();await f.initialize();await f.repo.start();const snapshot=await f.repo.snapshot(),manifest=JSON.parse(Buffer.from(snapshot[0].bytes));manifest.formatVersion=99;const future=[{name:'manifest.json',bytes:new Uint8Array(Buffer.from(JSON.stringify(manifest)))}];
  await assert.rejects(f.repo.restore(future,f.repo.revision()),/CONFIG_REQUIRES_UPDATE/);const revision=f.repo.revision();await f.settings.saveGlobalSettings(f.models.createDefaultGlobalSettings());
  await assert.rejects(f.repo.restore(snapshot,revision),/CONFIG_LOCAL_CHANGED/);const backup=path.join(f.library,'.pomelo/app-settings-backup.zip');fs.mkdirSync(path.dirname(backup),{recursive:true});const bytes=f.zip.encodeConfigurationZip(future);fs.writeFileSync(backup,bytes);await f.repo.backupLocal().catch(()=>{});assert.deepEqual(fs.readFileSync(backup),Buffer.from(bytes));f.dispose();
 });
 await test('ZIP CRC, traversal, extra members and truncation are rejected before applying',async()=>{
  const f=fixture();await f.initialize();const members=await f.repo.snapshot(),bytes=f.zip.encodeConfigurationZip(members);const broken=bytes.slice();broken[50]^=1;assert.throws(()=>f.zip.decodeConfigurationZip(broken));assert.throws(()=>f.zip.decodeConfigurationZip(bytes.slice(0,-1)));
  assert.throws(()=>f.zip.encodeConfigurationZip([{name:'../x',bytes:new Uint8Array([1])}]));await assert.rejects(f.repo.validateMembers([...members,{name:'assets/'+ 'f'.repeat(64)+'.img',bytes:new Uint8Array([1])}]),/CONFIG_INVALID/);f.dispose();
 });
 await test('deleted game directories are never recreated while saving a shared profile',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'removed');fs.mkdirSync(game);const exe=path.join(game,'g.exe');fs.writeFileSync(exe,'g');const id=await f.portable.discover(game,exe,'old');fs.rmSync(game,{recursive:true});
  const value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.arguments=['new'];await f.settings.saveAppSettings(value);assert.equal(fs.existsSync(game),false);f.dispose();
 });
 await test('multiple executables retain distinct dedicated layouts and launch options',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'multi');fs.mkdirSync(game);const a=path.join(game,'a.exe'),b=path.join(game,'b.exe');fs.writeFileSync(a,'a');fs.writeFileSync(b,'b');
  const id=await f.portable.discover(game,a,'old');let value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.executable=a;value.arguments=['A'];value.inputProfileId='app-a';let profile=f.models.createDefaultInputProfile();profile.id='app-a';profile.cursorSpeed=1.3;await f.profiles.saveProfile(profile);await f.settings.saveAppSettings(value);
  value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.executable=b;await f.settings.saveAppSettings(value);value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.arguments=['B'];profile=f.profiles.getProfile(value.inputProfileId);profile.cursorSpeed=2.1;await f.profiles.saveProfile(profile);await f.settings.saveAppSettings(value);await f.portable.flush();
  value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.executable=a;await f.settings.saveAppSettings(value);value=f.settings.getAppSettings(id);same(value.arguments,['A']);assert.equal(f.profiles.getProfile(value.inputProfileId).cursorSpeed,1.3);
  const side=JSON.parse(fs.readFileSync(path.join(game,'.pomelo/game.json')));assert.equal(side.entries.length,2);assert.equal(side.entries.find(e=>e.executable==='b.exe').input.profile.cursorSpeed,2.1);
  value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.executable=b;await f.settings.saveAppSettings(value);assert.equal(await f.portable.discover(game,a,'old'),id);assert.equal(f.settings.getAppSettings(id).executable,b);
  const reinstalled=fixture({library:f.library,filesDir:path.join(path.dirname(f.context.filesDir),'multiReinstall')});await reinstalled.initialize();assert.equal(await reinstalled.portable.discover(game,a,'old'),id);assert.equal(reinstalled.settings.getAppSettings(id).executable,b);assert.equal(reinstalled.profiles.getProfile(reinstalled.settings.getAppSettings(id).inputProfileId).cursorSpeed,2.1);f.dispose();
 });
 await test('simultaneous copies get distinct portable identities while retaining all controls',async()=>{
  const f=fixture();await f.initialize();const original=path.join(f.library,'original');fs.mkdirSync(original);fs.writeFileSync(path.join(original,'g.exe'),'g');const a=await f.portable.discover(original,path.join(original,'g.exe'),'a');
  const copy=path.join(f.library,'copy');fs.cpSync(original,copy,{recursive:true});const b=await f.portable.discover(copy,path.join(copy,'g.exe'),'b');assert.notEqual(a,b);assert.ok(b.length<128);assert.notEqual(JSON.parse(fs.readFileSync(path.join(original,'.pomelo/game.json'))).gameUid,JSON.parse(fs.readFileSync(path.join(copy,'.pomelo/game.json'))).gameUid);f.dispose();
 });
 await test('unchanged directory refresh never rewrites the fallback index',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'idle');fs.mkdirSync(game);const exe=path.join(game,'g.exe');fs.writeFileSync(exe,'g');await f.portable.discover(game,exe,'old');
  const store=f.getStore('pomelo_game_config'),flushes=store.flushes;for(let i=0;i<8;i++)await f.portable.discover(game,exe,'old');assert.equal(store.flushes,flushes);f.dispose();
 });
 await test('crashed restore rolls back settings and fonts before consumers initialize',async()=>{
  const a=fixture();await a.initialize();await editScale(a,65);const fonts=path.join(a.context.filesDir,'.wine/drive_c/windows/fonts');fs.mkdirSync(fonts,{recursive:true});fs.writeFileSync(path.join(fonts,'test.ttf'),'newfont');await a.repo.recordFonts(['test.ttf'],fonts);const remote=await a.repo.snapshot();
  let fail=false;const b=fixture({failFlush:name=>fail&&name==='input_settings'});await b.initialize();await editScale(b,81);const dest=path.join(b.context.filesDir,'.wine/drive_c/windows/fonts');fs.mkdirSync(dest,{recursive:true});fs.writeFileSync(path.join(dest,'test.ttf'),'oldfont');fail=true;
  await assert.rejects(b.repo.restore(remote,b.repo.revision()),/INTERRUPTED/);assert.ok(fs.existsSync(path.join(b.context.filesDir,'user_config/restore-journal.json')));fail=false;
  const restarted=fixture({filesDir:b.context.filesDir,library:b.library,persistent:b.stores});await restarted.initialize();assert.equal(restarted.settings.getGlobalSettings().outputScale,81);assert.equal(fs.readFileSync(path.join(dest,'test.ttf'),'utf8'),'oldfont');assert.equal(fs.existsSync(path.join(dest,'../user_config/restore-journal.json')),false);a.dispose();b.dispose();
 });
 await test('a local edit during restore survives rollback instead of being silently overwritten',async()=>{
  const a=fixture();await a.initialize();await editScale(a,65);const remote=await a.repo.snapshot();let b,once=false;
  b=fixture({failFlush:async name=>{if(name==='input_settings'&&once){once=false;const p=b.getStore('vintage_pomelo_pro'),v=JSON.parse(p.getSync('global_settings_v1','{}'));v.outputScale=93;await p.put('global_settings_v1',JSON.stringify(v));await p.flush();}return false;}});
  await b.initialize();await b.repo.start();await editScale(b,81);once=true;await assert.rejects(b.repo.restore(remote,b.repo.revision()),/CONFIG_LOCAL_CHANGED/);b.settings.reloadConfiguration();assert.equal(b.settings.getGlobalSettings().outputScale,93);assert.ok(b.repo.hasLocalUserData());a.dispose();b.dispose();
 });
 await test('complete cloud backup deduplicates unchanged wallpaper and restores every setting',async()=>{
  const c=await cloudFixture(),a=fixture({cloudApi:c.api});await a.initialize();await a.repo.start();await editScale(a,77);const picture=path.join(a.library,'wall.img');fs.writeFileSync(picture,Buffer.alloc(300000,8));const pathTo=await a.repo.importWallpaper(picture);const settings=a.settings.getGlobalSettings();settings.pageBackgroundUri=pathTo;settings.usePageBackground=true;await a.settings.saveGlobalSettings(settings);
  const cloud=await client(a,c);assert.equal(cloud.usedBytes(),-1);assert.equal(await cloud.sync(()=>{}),'云端备份已更新');assert.ok(cloud.usedBytes()>0);const uploads=c.calls.filter(x=>x.method==='PUT').length,head=(await c.service.current(c.api.getUserId(),'desktop')).head;
  c.calls.length=0;assert.equal(await cloud.sync(()=>{}),'云端备份已更新');assert.equal(c.calls.length,1);assert.equal(c.calls[0].method,'GET');
  await editScale(a,79);const global=a.settings.getGlobalSettings();global.pageBackgroundUri=pathTo;await a.settings.saveGlobalSettings(global);c.calls.length=0;await cloud.sync(()=>{});assert.equal(c.calls.filter(x=>x.method==='PUT').length,1);assert.equal(c.calls.filter(x=>x.route.endsWith('/lease')).length,0);
  const b=fixture({cloudApi:c.api});await b.initialize();await b.repo.start();const restored=await client(b,c);c.calls.length=0;assert.equal(await restored.sync(()=>{}),'已恢复应用配置');b.settings.reloadConfiguration();assert.equal(b.settings.getGlobalSettings().outputScale,79);assert.deepEqual(fs.readFileSync(b.settings.getGlobalSettings().pageBackgroundUri),Buffer.alloc(300000,8));assert.equal(c.calls.filter(x=>x.route.includes('/chunks/')).length,3);assert.ok(uploads>=3);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.revision,head.revision+1);a.dispose();b.dispose();
 });
 await test('interrupted cloud upload preserves published head and retries the pending local edit',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,75);const cloud=await client(f,c);await cloud.sync(()=>{});const old=(await c.service.current(c.api.getUserId(),'desktop')).head;
  await editScale(f,81);c.hook((route,method)=>{if(method==='PUT')throw Error('OFFLINE');});await assert.rejects(cloud.sync(()=>{}),/OFFLINE/);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.revision,old.revision);
  c.hook(null);await cloud.sync(()=>{});assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.revision,old.revision+1);assert.ok(c.calls.some(x=>x.route.endsWith('/abort')));f.dispose();
 });
 await test('a restarted client recovers an interrupted upload already removed by cloud cleanup',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,75);const cloud=await client(f,c);await cloud.sync(()=>{});const old=(await c.service.current(c.api.getUserId(),'desktop')).head;
  await editScale(f,81);c.hook((route,method)=>{if(method==='PUT')throw Error('OFFLINE');});await assert.rejects(cloud.sync(()=>{}),/OFFLINE/);
  const journal=JSON.parse(f.getStore('configuration_cloud_journal').getSync('state_v1','{}'));assert.ok(journal.pendingId);c.hook(null);c.service.clock=()=>Date.now()+3600001;await c.service.cleanup(c.api.getUserId());assert.equal(await c.store.get('config-job:'+journal.pendingId),null);
  assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.sha256,old.sha256);
  const restarted=fixture({cloudApi:c.api,library:f.library,filesDir:f.context.filesDir,persistent:f.stores});await restarted.initialize();await restarted.repo.start();const resumed=await client(restarted,c);
  assert.equal(await resumed.sync(()=>{}),'云端备份已更新');assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.revision,old.revision+1);assert.equal(restarted.settings.getGlobalSettings().outputScale,81);
  assert.equal(JSON.parse(restarted.getStore('configuration_cloud_journal').getSync('state_v1','{}')).pendingId,'');f.dispose();
 });
 await test('pending upload cleanup preserves the retry record for network and authorization failures',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,75);const cloud=await client(f,c);await cloud.sync(()=>{});const old=(await c.service.current(c.api.getUserId(),'desktop')).head;
  await editScale(f,81);c.hook((route,method)=>{if(method==='PUT')throw Error('OFFLINE');});await assert.rejects(cloud.sync(()=>{}),/OFFLINE/);
  const journal=f.getStore('configuration_cloud_journal'),pending=journal.getSync('state_v1','');
  for(const code of ['OFFLINE','SIGN_IN_REQUIRED','MEMBERSHIP_REQUIRED','SERVICE_ERROR']){
   c.hook(route=>{if(route.endsWith('/abort'))throw Error(code);});await assert.rejects(cloud.sync(()=>{}),new RegExp(code));assert.equal(journal.getSync('state_v1',''),pending);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head.sha256,old.sha256);
  }f.dispose();
 });
 await test('an account change during missing-upload cleanup cannot clear the previous users journal',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,75);const cloud=await client(f,c);
  c.hook((route,method)=>{if(method==='PUT')throw Error('OFFLINE');});await assert.rejects(cloud.sync(()=>{}),/OFFLINE/);
  const journal=f.getStore('configuration_cloud_journal'),pending=journal.getSync('state_v1',''),owner=c.api.getUserId();
  c.hook(route=>{if(route.endsWith('/abort')){c.owner('b'.repeat(64));throw Error('CONFIG_NOT_FOUND');}});
  await assert.rejects(cloud.sync(()=>{if(c.api.getUserId()!==owner)throw Error('ACCOUNT_CHANGED');}),/ACCOUNT_CHANGED/);assert.equal(journal.getSync('state_v1',''),pending);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head,null);f.dispose();
 });
 await test('edits during upload are not published and are queued for a complete retry',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,70);const cloud=await client(f,c);let once=true;
  c.hook(async(route,method)=>{if(method==='PUT'&&once){once=false;await editScale(f,91);}});await assert.rejects(cloud.sync(()=>{}),/CONFIG_LOCAL_CHANGED/);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head,null);
  c.hook(null);await cloud.sync(()=>{});const b=fixture({cloudApi:c.api});await b.initialize();await client(b,c).then(x=>x.sync(()=>{}));b.settings.reloadConfiguration();assert.equal(b.settings.getGlobalSettings().outputScale,91);f.dispose();b.dispose();
 });
 await test('account switch and legacy ownership never upload the previous users configuration',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,72);const cloud=await client(f,c);await cloud.sync(()=>{});assert.ok(cloud.usedBytes()>0);c.owner('b'.repeat(64));assert.equal(cloud.usedBytes(),-1);c.calls.length=0;
  assert.match(await cloud.sync(()=>{}),/其他账号/);assert.equal(c.calls.filter(x=>x.method!=='GET').length,0);assert.equal((await c.service.current(c.api.getUserId(),'desktop')).head,null);
  const migrated=fixture({cloudApi:c.api});await migrated.initialize();await migrated.repo.start();await editScale(migrated,83);const migration=await client(migrated,c,'a'.repeat(64));assert.match(await migration.sync(()=>{}),/其他账号/);f.dispose();migrated.dispose();
 });
 await test('unsupported future cloud configuration is preserved even when keeping local data is requested',async()=>{
  const c=await cloudFixture(),f=fixture({cloudApi:c.api});await f.initialize();await f.repo.start();await editScale(f,72);const cloud=await client(f,c);await cloud.sync(()=>{});
  const snap=await f.repo.snapshot(),value=JSON.parse(Buffer.from(snap[0].bytes));value.formatVersion=99;const bytes=Buffer.from(JSON.stringify(value)),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
  const manifest={formatVersion:1,deviceClass:'desktop',totalBytes:bytes.length,members:[{name:'manifest.json',size:bytes.length,sha256:hash(bytes),chunks:[{sha256:hash(bytes),size:bytes.length}]}]};
  const original=c.api.configurationRequest;let writes=0;
  c.api.configurationRequest=async(route,method,raw)=>{if(route==='/v1/configuration/desktop')return {head:{id:crypto.randomUUID(),revision:2,sha256:hash(JSON.stringify(manifest)),totalBytes:bytes.length,createdAt:1},manifest,quotaBytes:67108864};if(route.endsWith('/lease'))return {status:'LEASED'};if(route.includes('/chunks/'))return {sha256:hash(bytes),data:bytes.toString('base64')};writes++;return original(route,method,raw);};
  await assert.rejects(cloud.sync(()=>{},'local'),/CONFIG_REQUIRES_UPDATE/);assert.equal(writes,0);assert.equal(f.settings.getGlobalSettings().outputScale,72);f.dispose();
 });
 await test('a same-format optional setting survives normal edits and legacy imported font bytes migrate',async()=>{
  const f=fixture();const p=f.getStore('vintage_pomelo_pro'),value={...f.models.createDefaultGlobalSettings(),futureOptional:{keep:'value'}};await p.put('global_settings_v1',JSON.stringify(value));await p.flush();
  const fonts=path.join(f.context.filesDir,'.wine/drive_c/windows/fonts');fs.mkdirSync(fonts,{recursive:true});fs.writeFileSync(path.join(fonts,'Legacy.ttf'),'legacy user font');await f.initialize();await editScale(f,64);
  assert.equal(JSON.parse(p.getSync('global_settings_v1','{}')).futureOptional.keep,'value');const snapshot=await f.repo.snapshot();assert.ok(JSON.parse(Buffer.from(snapshot[0].bytes)).assets.some(a=>a.target==='font:Legacy.ttf'));f.dispose();
 });
 await test('portable identity migration preserves favorites and play time without duplicate historical IDs',async()=>{
  const f=fixture();await f.initialize();const settings=f.models.createDefaultAppSettings('old');settings.favorite=true;await f.settings.saveAppSettings(settings);await f.settings.setPlayMinutes('old',80);await f.settings.remapAppId('old','new');
  assert.equal(f.settings.getPlayMinutes('old'),0);assert.equal(f.settings.getPlayMinutes('new'),80);assert.equal(f.settings.getAppSettings('new').favorite,true);await f.settings.remapAppId('old','new');assert.equal(f.settings.getPlayMinutes('new'),80);f.dispose();
 });
 await test('generic engine EXEs cannot associate a different game with a deleted games fallback',async()=>{
  const f=fixture();await f.initialize();const a=path.join(f.library,'A'),b=path.join(f.library,'B');fs.mkdirSync(a);fs.writeFileSync(path.join(a,'game.exe'),'shared engine');fs.writeFileSync(path.join(a,'game.dat'),'game A');
  const first=await f.portable.discover(a,path.join(a,'game.exe'),'old');let value=f.models.cloneAppSettings(f.settings.getAppSettings(first));value.arguments=['only-A'];await f.settings.saveAppSettings(value);fs.rmSync(a,{recursive:true});
  fs.mkdirSync(b);fs.writeFileSync(path.join(b,'game.exe'),'shared engine');fs.writeFileSync(path.join(b,'game.dat'),'game B');const second=await f.portable.discover(b,path.join(b,'game.exe'),'new');assert.notEqual(first,second);same(f.settings.getAppSettings(second).arguments,[]);
  const recovered=path.join(f.library,'A recovered');fs.mkdirSync(recovered);fs.writeFileSync(path.join(recovered,'game.exe'),'shared engine');fs.writeFileSync(path.join(recovered,'game.dat'),'game A');assert.equal(await f.portable.discover(recovered,path.join(recovered,'game.exe'),'restored'),first);same(f.settings.getAppSettings(first).arguments,['only-A']);f.dispose();
 });
 await test('sidecar reimports and executable switches retain accurate fallback content identities',async()=>{
  const f=fixture();await f.initialize();const game=path.join(f.library,'portable');fs.mkdirSync(game);fs.writeFileSync(path.join(game,'first.exe'),'first executable');fs.writeFileSync(path.join(game,'second.exe'),'second executable');fs.writeFileSync(path.join(game,'game.dat'),'unique game content');
  const id=await f.portable.discover(game,path.join(game,'first.exe'),'old');let value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.arguments=['first'];value.executable=path.join(game,'first.exe');await f.settings.saveAppSettings(value);
  value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.arguments=['second'];value.executable=path.join(game,'second.exe');await f.settings.saveAppSettings(value);
  value=f.models.cloneAppSettings(f.settings.getAppSettings(id));value.executable=path.join(game,'first.exe');await f.settings.saveAppSettings(value);
  const g=fixture({library:f.library,filesDir:path.join(path.dirname(f.context.filesDir),'reimported')});await g.initialize();assert.equal(await g.portable.discover(game,path.join(game,'first.exe'),'reimport'),id);
  const store=g.getStore('pomelo_game_config'),index=JSON.parse(store.getSync('index_v1','[]'));assert.equal(index[0].executableSha256,crypto.createHash('sha256').update('first executable').digest('hex'));assert.match(index[0].contentFingerprint,/^[a-f0-9]{64}$/);
  const writes=store.flushes;await g.portable.discover(game,path.join(game,'first.exe'),id);assert.equal(store.flushes,writes);
  const recovered=path.join(f.library,'recovered');fs.renameSync(game,recovered);fs.rmSync(path.join(recovered,'.pomelo'),{recursive:true});assert.equal(await g.portable.discover(recovered,path.join(recovered,'first.exe'),'copy'),id);same(g.settings.getAppSettings(id).arguments,['first']);f.dispose();
 });
 console.log('PASS configuration integration scenarios: '+count);
})().catch(e=>{console.error(e);process.exitCode=1;});
