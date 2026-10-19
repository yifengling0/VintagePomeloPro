// Exercise the production scanner and session adapter with real host files.
const fs=require('node:fs'), path=require('node:path'), os=require('node:os'), vm=require('node:vm');
const assert=require('node:assert/strict'), ts=require(process.argv[2]||'typescript');
const root=path.resolve(__dirname,'../entry/src/main/ets');
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'steam-local-'));
const settings=new Map(), processes=[], killed=[], stopped=[], launches=[];
let mode='ready', catalogCache=[];
const fileIo={accessSync:fs.existsSync, statSync:fs.statSync,lstatSync:fs.lstatSync,
  stat:fs.promises.stat, listFile:fs.promises.readdir, listFileSync:p=>fs.readdirSync(p),readTextSync:p=>fs.readFileSync(p,'utf8'),mkdirSync:fs.mkdirSync,
 OpenMode:{CREATE:64,TRUNC:512,WRITE_ONLY:1},openSync:p=>({fd:fs.openSync(p,'w')}),
 writeSync:(fd,v)=>fs.writeSync(fd,typeof v==='string'?v:Buffer.from(v)),fsyncSync:fs.fsyncSync,closeSync:f=>fs.closeSync(f.fd),rename:fs.promises.rename,unlink:fs.promises.unlink};
const native={getProcessList:()=>processes.slice(),getWineSession:()=>null,getDesktopRootId:()=>0,
 killProcess:p=>{killed.push(p);return true;},stopWineSession:s=>{stopped.push(s);return true;},activateWineSession(){},raiseToplevel(){}};
const store={getAppSettings:id=>settings.get(id)||{customCover:'',customTitle:'',arguments:[],displayModeOverride:null,box64Preset:'default'},
 getCatalogCache:()=>catalogCache,saveCatalogCache:async cards=>{catalogCache=cards;},
 recordRecentLaunch:async()=>{},usesManagedWineWindows:()=>true,getGlobalSettings:()=>({})};
let local, models;
const engine={getState:()=>models.EngineState.READY,isReady:()=>true,
 getPresentationMode:()=>models.WinePresentationMode.MANAGED_WINDOWS,
 selectHostGraphicsBackendForExecutable:()=>'',needsHostGraphicsRestart:()=>false,
 launchExecutable:(exe,args)=>{launches.push({exe,args});return {pid:99,sessionId:'command'};}};
const mocks={
 '@ohos.util':{}, '@ohos.bundle.bundleManager':{},
 './DesktopModeService':{DesktopModeService:{getInstance:()=>({getOverrideMode:()=>'',mode:'virtual'})},MODE_VIRTUAL:'virtual'},
 '../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{isPhoneDevice:()=>true}},
 '@kit.BasicServicesKit':{},
 '@kit.AbilityKit':{},'@kit.CoreFileKit':{fileIo,picker:{}},'@kit.ArkData':{preferences:{}},
 'libentry.so':{default:native},'./SteamMatch':{SteamMatch:{}},
 '../service/AppSettingsStore':{AppSettingsStore:{getInstance:()=>store}},
 './AppSettingsStore':{AppSettingsStore:{getInstance:()=>store}},
 '../service/GameContainer':{wineDriveCForContainer:()=>'/prefix/drive_c'},
 './LogService':{hilog:{info(){},warn(){},error(){}}},
 './PortableGameConfigurationService':{PortableGameConfigurationService:{getInstance:()=>({discover:async(_directory,_exe,id)=>id})}},
 '../steam/SteamLocalService':{SteamLocalService:{getInstance:()=>local}},
 './WineEngineService':{WineEngineService:{getInstance:()=>engine}},
 './WineWindowManager':{WineWindowManager:{getInstance:()=>({isDesktopMode:false})}},
 './InputOverlayWindowCoordinator':{}, './AppCatalogService':{}
};
const cache=new Map();
function load(file){
 file=path.resolve(root,file);if(cache.has(file))return cache.get(file);
 const exports={}; cache.set(file,exports);
 vm.runInNewContext(ts.transpileModule(fs.readFileSync(file,'utf8'),{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2021}}).outputText,
 {exports, Map,Set,Date,Promise,ArrayBuffer,Uint8Array,setTimeout:()=>0,require(name){
 if(Object.hasOwn(mocks,name))return mocks[name];assert.ok(name.startsWith('.'),'Unexpected dependency '+name);
 return load(path.resolve(path.dirname(file),name+'.ets'));
 }},{filename:file});return exports;
}
function put(p,content=''){fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,content);}
function manifest(lib,id,name,flags=4){put(lib+'/steamapps/appmanifest_'+id+'.acf',`"AppState" {"appid" "${id}" "name" "${name}" "installdir" "${name}" "StateFlags" "${flags}"}`);}
(async()=>{
 models=load('model/AppModels.ets');
 local=new (load('steam/SteamLocalService.ets').SteamLocalService)();local.root=tmp;
 const client=tmp+'/Steam', second=tmp+'/游戏 库';
 put(client+'/steam.exe');put(client+'/steamapps/common/中文 游戏/game.exe');
 put(second+'/steamapps/common/另一个/game.exe');
 put(client+'/steamapps/libraryfolders.vdf','"libraryfolders" {"0" {"path" "Z:\\\\Steam"} "1" {"path" "Z:\\\\游戏 库"} "2" {"path" "Q:\\\\unavailable"}}');
 manifest(client,'123','中文 游戏');manifest(client,'456','半安装',1026);
 manifest(second,'123','另一个',1026);manifest(second,'789','另一个');
 settings.set('steam-default-123',{customCover:'/my/cover.jpg',arguments:['game arg'],customTitle:'自定义',displayModeOverride:null,box64Preset:'default'});
 let cards=await local.scan();assert.equal(cards.length,4);assert.equal(cards.filter(c=>c.steam.appId==='123').length,1);
 const game=local.getGame('123');assert.equal(game.available,true);assert.equal(game.coverPath,'/my/cover.jpg');assert.equal(game.title,'自定义');
 assert.equal(local.getGame('456').available,false);assert.equal(local.getGame('789').available,true);
 assert.equal(await local.libraryChanged(),true);assert.equal(await local.libraryChanged(),false);
 put(client+'/steamapps/appmanifest_123.acf','"AppState" {');
 cards=await local.scan();assert.equal(local.getGame('123').available,true,'partial write preserves valid prior card');
 fs.unlinkSync(second+'/steamapps/appmanifest_789.acf');await local.scan();assert.equal(local.getGame('789'),null,'uninstall removes card');
 manifest(client,'123','中文 游戏');await local.scan();
 const old=tmp+'/games/Steam-legacy/Steam';put(old+'/steam.exe');
 put(old+'/steamapps/common/旧 游戏/game.exe');manifest(old,'777','旧 游戏');manifest(old,'228980','Steamworks Shared');
 await local.normalizeLibraryFolders(client);
 await local.scan();assert.equal(local.getGame('777').available,true,'old client library registered in place');
 assert.equal(local.getGame('228980'),null,'redistributables must not be launchable game cards');
 assert.ok(fs.existsSync(old+'/steamapps/common/旧 游戏/game.exe'),'legacy files retained');
 assert.equal(local.isLibraryContainer(tmp+'/games/Steam-legacy'),true);
 assert.equal(local.getGame('123').coverPath,'/my/cover.jpg');
 processes.push({pid:1,path:'Z:\\Steam\\steam.exe',state:'running'},
 {pid:2,path:'Z:\\Steam\\steamapps\\common\\中文 游戏\\game.exe',state:'running'},
 {pid:3,path:'Z:\\Steam\\steamapps\\common\\中文 游戏\\UnityCrashHandler64.exe',state:'running'});
 assert.deepEqual(Array.from(local.processesFor(game,processes),p=>p.pid),[2]);
 assert.equal(local.hasRunningProcesses(),true);assert.equal(local.hasForeignClient(),false);
 processes.push({pid:4,path:'C:\\OldSteam\\steam.exe',state:'running'});assert.equal(local.hasForeignClient(),true);processes.pop();
 put(client+'/bin/existing.dll');local.context={resourceManager:{getRawFileContent:async()=>new Uint8Array(1024).fill(13)}};
 await local.ensureGpuDependencies();assert.equal(fs.statSync(client+'/bin/SDL2.dll').size,1024);
 put(client+'/bin/SDL2.dll','provided-by-package');await local.ensureGpuDependencies();assert.equal(fs.readFileSync(client+'/bin/SDL2.dll','utf8'),'provided-by-package');
 local.context=null;
 const service=new (load('service/AppSessionService.ets').AppSessionService)();
 service.getRunningSnapshot();assert.ok(service.getRunningAppIds().has(game.id));
 service.associateToplevel('',2,77);assert.equal(service.findByAppId(game.id).toplevelId,77);
 service.stop(service.findByAppId(game.id));assert.deepEqual(killed,[2]);assert.deepEqual(stopped,[],'game stop must not kill shared Steam session');
 processes.splice(1); // Steam remains running while next game starts.
 const first=service.launch(game,{},tmp),secondLaunch=service.launch(game,{},tmp);
 const result=await first;assert.equal(result.state,models.SessionState.STARTING);assert.equal(result.pid,0);
 await secondLaunch;assert.equal(launches.length,1,'coalesce concurrent clicks');
 const args=Array.from(launches[0].args);assert.deepEqual(args.slice(args.indexOf('-applaunch')),['-applaunch','123','game arg']);
 await service.launch(game,{},tmp);assert.equal(launches.length,1,'coalesce request awaiting actual game');
 assert.equal(service.getRunningAppIds().has(game.id),false,'Steam command sender is not a running game');
 processes.push({pid:5,path:'Z:\\Steam\\steamapps\\common\\中文 游戏\\game.exe',state:'running'});
 assert.equal(service.getRunningAppIds().has(game.id),true);
 processes[1].state='exited';assert.equal(service.getRunningAppIds().has(game.id),false);
 assert.ok(service.getRunningAppIds().has('steam-client-default'),'game exit preserves Steam');
 await service.launch(local.clientCard(),{},tmp);await service.launch(local.clientCard(),{},tmp);
 assert.equal(launches.length,3,'every open-client action reaches the running silent client');
 put(client+'/2-perf-gpu-auto.bat','@echo off\ncd /d "%~dp0"\nstart steam.exe');
 const batchCard=local.clientCard();
 assert.equal(batchCard.launchTarget.executable,client+'/2-perf-gpu-auto.bat');
 assert.equal(local.installCard('555').launchTarget.executable,client+'/steam.exe','install URI must reach Steam directly');
 await service.launch(batchCard,{},tmp);
 assert.equal(launches.at(-1).exe,client+'/steam.exe','warm client bypasses batch');
 assert.equal(launches.at(-1).args.at(-1),'steam://open/main');
 processes.length=0;
 await service.launch(batchCard,{},tmp);
 assert.equal(launches.at(-1).exe,client+'/2-perf-gpu-auto.bat','cold card uses the bundled 2 launcher');
 assert.equal(launches.at(-1).args.length,0,'batch owns its flags');
 local.compatibility=true;
 await service.launch(batchCard,{},tmp);
 assert.equal(launches.at(-1).exe,client+'/steam.exe','compatibility changes apply even to a cached batch card');
 assert.ok(launches.at(-1).args.includes('-cef-disable-gpu'));
 local.compatibility=false;
 fs.unlinkSync(client+'/2-perf-gpu-auto.bat');
 await service.launch(batchCard,{},tmp);
 assert.equal(launches.at(-1).exe,client+'/steam.exe','missing batch falls back even with a cached card');
 processes.push({pid:1,path:'Z:\\Steam\\steam.exe',state:'running'});
 // Exercise production ordinary discovery and cache reconciliation together.
 const catalog=new (load('service/AppCatalogService.ets').AppCatalogService)();
 catalog.libraryRoot=tmp;
 const ordinary=tmp+'/games/ATRI', missing=tmp+'/games/缺少主程序';
 put(ordinary+'/ATRI.exe');put(missing+'/cover.png');
 const sibling=tmp+'/games/Steam-legacy/普通 游戏';put(sibling+'/game.exe');
 const copied=tmp+'/games/steam';put(copied+'/steam.exe');
 for(const folder of ['appcache','bin','clientui','config','controller_base','depotcache','steamapps/downloading']) put(copied+'/'+folder+'/helper.dll');
 put(copied+'/bin/helper.exe');put(copied+'/steamapps/common/后加入 游戏/play.exe');manifest(copied,'888','后加入 游戏');
 const download=copied+'/999';put(download+'/download.exe');
 put(download+'/.steam-game.json',JSON.stringify({source:'steam',appId:999,name:'旧下载游戏'}));
 const mixed=tmp+'/games/混合库';put(mixed+'/steamapps/common/库内 游戏/game.exe');manifest(mixed,'1001','库内 游戏');
 put(mixed+'/普通游戏/config/game.exe');
 put(client+'/steamapps/libraryfolders.vdf','"libraryfolders" {"0" {"path" "Z:\\\\Steam"} "1" {"path" "Z:\\\\games\\\\混合库"}}');
 for(const path of [client+'/appcache',tmp+'/.steam-import-stage/bin',tmp+'/.steam-import-backup/config',tmp+'/games/steam-download-staging/999']) put(path+'/old.dll');
 function cached(dir,id,exe=''){return {id,title:id,source:models.AppSource.DOWNLOAD,directory:dir,coverPath:'',available:false,
  launchTarget:{kind:models.LaunchKind.WINE_EXE,executable:exe,arguments:[],workingDirectory:dir},displayModeOverride:null};}
 const bad=[copied+'/appcache',copied+'/bin',copied+'/clientui',copied+'/config',copied+'/controller_base',
  copied+'/depotcache',copied+'/steamapps/downloading',old+'/steamapps/common/旧 游戏',client+'/appcache',
  tmp+'/.steam-import-stage/bin',tmp+'/.steam-import-backup/config',tmp+'/games/steam-download-staging/999',
  tmp+'/games/Steam-legacy',mixed,mixed+'/steamapps/common/库内 游戏'];
 catalogCache=bad.map((dir,i)=>cached(dir,'bad-'+i));
 catalogCache.push(cached(ordinary,'user-atri',ordinary+'/ATRI.exe'),cached(missing,'user-missing'));
 settings.set('user-atri',{customCover:'/user/ATRI.png',customTitle:'自选 ATRI',arguments:[],executable:'',workingDirectory:'',displayModeOverride:null});
 let found=await catalog.scan();
 assert.equal(found.some(c=>c.id.startsWith('bad-')),false,'Steam internals and wrappers cannot resurrect from cache');
 assert.equal(found.filter(c=>c.steam?.state==='client').length,1,'one managed client');
 assert.equal(found.filter(c=>c.steam?.appId==='888').length,1,'late legacy library is discovered without reimport');
 assert.equal(found.filter(c=>c.steam?.appId==='777').length,1,'legacy manifest game has one stable Steam card');
 assert.equal(found.find(c=>c.id==='user-atri').coverPath,'/user/ATRI.png','custom art and identity preserved');
 assert.equal(found.find(c=>c.id==='user-missing').available,false,'ordinary missing EXE card retained');
 assert.ok(found.some(c=>c.directory===sibling),'ordinary sibling of nested client retained');
 assert.ok(found.some(c=>c.directory===mixed+'/普通游戏'),'ordinary sibling in mixed library retained');
 assert.equal(found.find(c=>c.directory===download).title,'旧下载游戏','legacy downloader game retained even beside client');
 assert.equal(found.some(c=>bad.includes(c.directory)&&!c.steam),false,'no fresh helper or duplicate game cards');
 const firstIds=found.map(c=>c.id).sort();found=await catalog.scan();
 assert.deepEqual(found.map(c=>c.id).sort(),firstIds,'refresh preserves identities and removes stale cache');
 const rule=load('steam/SteamLocalRules.ets'),scope=local.getCatalogScope();
 assert.equal(rule.steamCatalogPathKind(copied.toUpperCase().replaceAll('/','\\')+'\\BIN\\',scope),'excluded','slash case and trailing separator');
 assert.equal(rule.steamCatalogPathKind(copied+'-ordinary',scope),'game','path prefix sibling stays a game');
 fs.unlinkSync(client+'/steam.exe');found=await catalog.scan();
 assert.equal(found.some(c=>bad.includes(c.directory)),false,'missing managed entry does not revive helper cache');
 assert.ok(found.some(c=>c.directory===download),'old downloads remain without managed client');
 put(client+'/steam.exe');
 console.log('Steam catalogue: managed/legacy clients, cached helpers, Unicode libraries, late legacy discovery, mixed siblings, old downloads, missing EXE, identity and repeated refresh passed');
 console.log('Steam services: multi-library, Unicode, partial installs, uninstall, identity, artwork, process ownership, coalescing and warm-client requests passed');
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>fs.rmSync(tmp,{recursive:true,force:true}));
