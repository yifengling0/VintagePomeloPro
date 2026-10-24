// Exercise the shipping ArkTS downloader against real temporary files and a
// deterministic HTTP transport. No device, credentials or production data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'steam-download-'));
const sha = bytes => crypto.createHash('sha256').update(Buffer.from(bytes)).digest('hex');
function load(relative, deps, extra = {}) {
  const exports = {};
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets', relative), 'utf8');
  const code = ts.transpileModule(source, {compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}}).outputText;
  vm.runInNewContext(code, {exports, Error, ArrayBuffer, Uint8Array, setTimeout, clearTimeout,
    require(id) { assert.ok(id in deps, id); return deps[id]; }, ...extra});
  return exports;
}
const model = load('model/LinuxSteamDownloadModels.ets', {});
const real = p => typeof p === 'string' && p.startsWith('/data/storage/el2/base/files/steamohos-native') ?
  path.join(temporary, 'private', p.substring('/data/storage/el2/base/files/steamohos-native'.length)) :
  typeof p === 'string' && p.startsWith('/data/storage/el2/base/files/.wine/drive_c/SteamLinuxARM64') ?
    path.join(temporary, 'private-c/SteamLinuxARM64',p.substring('/data/storage/el2/base/files/.wine/drive_c/SteamLinuxARM64'.length)) : p;
const io = {
  OpenMode: {CREATE: fs.constants.O_CREAT, WRITE_ONLY: fs.constants.O_WRONLY, TRUNC: fs.constants.O_TRUNC,
    READ_ONLY: fs.constants.O_RDONLY, APPEND: fs.constants.O_APPEND, NOFOLLOW: fs.constants.O_NOFOLLOW},
  accessSync: p => fs.existsSync(real(p)), statSync: p => typeof p === 'number' ? fs.fstatSync(p) : fs.statSync(real(p)),
  lstatSync: p => fs.lstatSync(real(p)), listFileSync: p => fs.readdirSync(real(p)),
  openSync: (p, flags) => ({fd: fs.openSync(real(p), flags)}), closeSync: f => fs.closeSync(f.fd ?? f),
  writeSync: (fd, bytes, options = {}) => {assert.equal(options.offset,undefined,'OHOS writes append sequentially without relative offsets');
    return fs.writeSync(fd, Buffer.from(bytes), 0, options.length ?? bytes.byteLength, null);},
  readSync: (fd, bytes, options = {}) => fs.readSync(fd, Buffer.from(bytes), 0, options.length ?? bytes.byteLength, options.offset ?? null),
  truncateSync: (fd, n) => fs.ftruncateSync(fd, n), fsyncSync: fd => fs.fsyncSync(fd),
  unlinkSync: p => fs.unlinkSync(real(p)), renameSync: (a,b) => fs.renameSync(real(a),real(b)),
  rmdirSync: p => fs.rmdirSync(real(p)), copyFile: async(a,b) => fs.copyFileSync(real(a),real(b))
};
const config = {ConfigurationFiles: {fileHash: async p => sha(fs.readFileSync(real(p))), hash: async b => sha(b),
  read: p => fs.readFileSync(real(p)),
  text: b => Buffer.from(b).toString('utf8'), bytes: s => Buffer.from(s),
  json: (p,s) => { assert.notEqual(s,'','OHOS empty text encoding is not a safe reset record');fs.writeFileSync(real(p),s); }}};
let responses = [], calls = [];
const http = {RequestMethod: {GET: 'GET'}, createHttp() {
  const events = {}; let destroyed = false;
  return {on: (n,f) => {events[n]=f;}, off: n => {delete events[n];}, destroy: () => {destroyed=true;},
    async requestInStream(url, options) {
      calls.push(options.header.range || 'full');
      assert.ok(responses.length,'unexpected HTTP request');
      const r = responses.shift();
      const status = r.status ?? 200;
      events.headersReceive?.({...(r.noStatus ? {} : {'status-line': `HTTP/2 ${status}`}),
        'content-length': String((r.body || []).length), ...(r.range ? {'content-range': r.range} : {})});
      if(r.hang) {if(r.hook)setTimeout(r.hook,1);return new Promise(()=>{});}
      const emit = () => { if(!destroyed) events.dataReceive?.(Uint8Array.from(r.body || []).buffer); };
      if(r.early) {setTimeout(()=>{emit();events.dataEnd?.();},2);return status;}
      emit(); if(r.hook) r.hook();
      if(r.error) throw Object.assign(new Error('connection reset'),{code:2300056});
      events.dataEnd?.(); return status;
    }};
}};
const download = load('service/LinuxSteamComponentDownload.ets', {
  '@kit.NetworkKit': {http}, '@kit.CoreFileKit': {fileIo:io}, './ConfigurationFiles':config,
  '../model/LinuxSteamDownloadModels': model
}, {setTimeout: (f,ms) => setTimeout(f, Math.min(ms,5))});
const bytes = Buffer.from('official-valve-content');
const archive = {id:'official',name:'Official',url:'https://client-update.steamstatic.com/test.zip',bytes:bytes.length,sha256:sha(bytes),unpackedBytes:bytes.length};
async function run(name, initial, input, expectedRanges, expectedError) {
  const dest = path.join(temporary,name+'.zip');
  if(initial) {fs.writeFileSync(dest+'.part',initial);fs.writeFileSync(dest+'.part.append-v1',archive.sha256);}
  responses = input; calls=[];
  const task = new download.LinuxSteamComponentDownload();
  if(expectedError) await assert.rejects(task.fetch(archive,dest,()=>{}),e=>e.code===expectedError);
  else {await task.fetch(archive,dest,()=>{});assert.deepEqual(fs.readFileSync(dest),bytes);}
  assert.deepEqual(calls,expectedRanges,name);
  assert.equal(responses.length,0,name);
  return dest;
}
async function main() {
  const end = bytes.length-1;
  await run('resume',bytes.subarray(0,5),[{status:206,range:`bytes 5-${end}/${bytes.length}`,body:bytes.subarray(5)}],['bytes=5-']);
  await run('ignored-range',bytes.subarray(0,5),[{body:bytes}],['bytes=5-']);
  await run('early-status',null,[{body:bytes,early:true}],['full']);
  await run('ohos-no-status',null,[{body:bytes,noStatus:true}],['full']);
  await run('ohos-no-status-resume',bytes.subarray(0,5),[{status:206,noStatus:true,range:`bytes 5-${end}/${bytes.length}`,body:bytes.subarray(5)}],['bytes=5-']);
  await run('interrupted',null,[{body:bytes.subarray(0,5),error:true},{status:206,range:`bytes 5-${end}/${bytes.length}`,body:bytes.subarray(5)}],['full','bytes=5-']);
  await run('checksum',null,[{body:Buffer.alloc(bytes.length)},{body:bytes}],['full','full']);
  const denied = await run('not-found',bytes.subarray(0,5),[{status:404,body:Buffer.from('error')}],['bytes=5-'],'HTTP_404');
  assert.deepEqual(fs.readFileSync(denied+'.part'),bytes.subarray(0,5),'error body preserves partial');
  const deniedOhos = await run('ohos-not-found',bytes.subarray(0,5),[{status:404,noStatus:true,body:Buffer.from('error')}],['bytes=5-'],'HTTP_404');
  assert.deepEqual(fs.readFileSync(deniedOhos+'.part'),bytes.subarray(0,5));
  await run('wrong-range',bytes.subarray(0,5),Array.from({length:3},()=>({status:206,range:`bytes 0-${end}/${bytes.length}`,body:bytes})),['bytes=5-','bytes=5-','bytes=5-'],'RANGE');
  await run('stale-range',bytes.subarray(0,5),[{status:416},{body:bytes}],['bytes=5-','full']);
  const large=Buffer.alloc(4194304+32,7);
  responses=[{status:206,noStatus:true,range:`bytes 0-4194303/${large.length}`,body:large.subarray(0,4194304)},
    {status:206,noStatus:true,range:`bytes 4194304-${large.length-1}/${large.length}`,body:large.subarray(4194304)}];calls=[];
  await new download.LinuxSteamComponentDownload().fetch({...archive,bytes:large.length,sha256:sha(large)},path.join(temporary,'segments.zip'),()=>{});
  assert.deepEqual(calls,['bytes=0-4194303',`bytes=4194304-${large.length-1}`]);
  let paused = new download.LinuxSteamComponentDownload();
  const pausePath = path.join(temporary,'paused.zip');
  responses=[{body:bytes.subarray(0,5),hook:()=>paused.pause()}];calls=[];
  await assert.rejects(paused.fetch(archive,pausePath,()=>{}),e=>e.code==='PAUSED');
  responses=[{status:206,range:`bytes 5-${end}/${bytes.length}`,body:bytes.subarray(5)}];
  await new download.LinuxSteamComponentDownload().fetch(archive,pausePath,()=>{});
  responses=[];calls=[];
  await new download.LinuxSteamComponentDownload().fetch(archive,pausePath,()=>{});
  assert.equal(calls.length,0,'verified cache avoids network');
  const missingLocal=path.join(temporary,'missing-local.zip');
  await assert.rejects(new download.LinuxSteamComponentDownload().fetch(archive,missingLocal,()=>{},false),e=>e.code==='LOCAL_CACHE');
  fs.writeFileSync(missingLocal,Buffer.from('damaged-cache'));
  await assert.rejects(new download.LinuxSteamComponentDownload().fetch(archive,missingLocal,()=>{},false),e=>e.code==='LOCAL_CACHE');
  assert.equal(calls.length,0,'offline errors never silently fall back to network');
  paused = new download.LinuxSteamComponentDownload();
  responses=[{body:bytes,hang:true,hook:()=>paused.pause()}];
  await assert.rejects(paused.fetch(archive,path.join(temporary,'hung.zip'),()=>{}),e=>e.code==='PAUSED');
  const production = JSON.parse(fs.readFileSync(path.join(__dirname,'../entry/src/main/resources/rawfile/linux-steam/deployment.json'),'utf8'));
  model.validateLinuxSteamDeployment(production);
  assert.equal(production.archives[0].url,'rawfile://linux-steam/hmosdeck-gnu-arm64-v1.zip');
  assert.equal(production.downloadBytes,production.archives.filter(x=>x.url.startsWith('https:')).reduce((n,x)=>n+x.bytes,0));
  for(const mutate of [p=>p.files[0].path='../escape',p=>p.files[0].path='client/userdata/1/save',
    p=>p.files[0].zipPath='../other',p=>p.archives[1].url='https://evil.invalid/test',p=>p.files.push(p.files[0])]) {
    const p=structuredClone(production);mutate(p);assert.throws(()=>model.validateLinuxSteamDeployment(p));
  }
  const root=path.join(temporary,'SteamLinuxARM64');
  const content=new Map([
    ['release/runtime/steamrtarm64/steam',Buffer.from('ELF')],
    ['release/runtime/manifest.json',Buffer.from('{}')],
    ['release/runtime/ld-linux-aarch64.so.1',Buffer.from('loader')],
    ['client/steamui/index.html',Buffer.from('UI')],
    ['client/steamrtarm64/icudtl.dat',Buffer.from('ICU')]
  ]);
  const embedded=Buffer.from('embedded-zip'),official=Buffer.from('official-zip');
  const manifest=JSON.stringify({schemaVersion:1,layout:'SteamLinuxARM64',executionModel:'native_aarch64',
    files:[...content].filter(([p])=>p.startsWith('release/')).map(([p,b])=>({path:p.substring(8),sha256:sha(b),bytes:b.length}))})+'\n';
  const plan={schemaVersion:1,hostAbi:1,id:sha(manifest),clientBuild:'1',clientDataId:sha('client-index'),
    downloadBytes:official.length,requiredSpaceBytes:536880912,releaseManifest:manifest,archives:[
      {id:'gnu-runtime',name:'GNU',url:'rawfile://linux-steam/hmosdeck-gnu-arm64-v1.zip',bytes:embedded.length,sha256:sha(embedded),unpackedBytes:100},
      {id:'official',name:'Steam',url:'https://client-update.steamstatic.com/test.zip',bytes:official.length,sha256:sha(official),unpackedBytes:100}],
    files:[...content].map(([p,b])=>({path:p,archive:p.includes('ld-linux')?'gnu-runtime':'official',source:p,zipPath:p,bytes:b.length,sha256:sha(b)}))};
  const embeddedPath=path.join(temporary,'gnu.zip');fs.writeFileSync(embeddedPath,embedded);
  const bundle=Buffer.from('pinned-offline-bundle');
  const bundlePath=path.join(temporary,'offline.zip');fs.writeFileSync(bundlePath,bundle);
  const metadata={schemaVersion:1,id:plan.id,bytes:bundle.length,sha256:sha(bundle),fileName:'offline.zip'};
  let free=1, fetches=0, activates=0, corrupt=false, extractions=0;
  const dirs=p=>{fs.mkdirSync(real(p),{recursive:true});return true;};
  class Transfer {pause(){} async fetch(a,d,p,network=true){
    if(!network) {assert.equal(sha(fs.readFileSync(real(d))),a.sha256,'offline uses only verified local cache');}
    else {fetches++;fs.writeFileSync(real(d),official);}p(official.length);
  }}
  const retention=load('service/LinuxSteamReleaseRetention.ets',{'@kit.CoreFileKit':{fileIo:io},'./ConfigurationFiles':config});
  const pointer='/data/storage/el2/base/files/steamohos-native/external-root.path';
  const readText=p=>{try{return fs.readFileSync(real(p),'utf8');}catch{return '';}};
  const policy=load('service/LinuxSteamDownloadPolicy.ets',{
    '@kit.CoreFileKit':{fileIo:io},'./ConfigurationFiles':config,
    './ManagedSmokePayloadService':{ensureSmokeDirectory:dirs,readSmokeText:readText}
  });
  const policyRoot=path.join(temporary,'policy-client');
  const cfg=path.join(policyRoot,'client/.steam/steam/steam_dev.cfg');
  fs.mkdirSync(path.dirname(cfg),{recursive:true});
  const original='// user config\n@user_download_setting 7\n';
  fs.writeFileSync(cfg,original);
  policy.applyLinuxSteamDownloadPolicy(policyRoot);assert.equal(fs.readFileSync(cfg,'utf8'),original,'default leaves existing cfg untouched');
  policy.setLinuxSteamDownloadHttp1(true);policy.applyLinuxSteamDownloadPolicy(policyRoot);
  const forced=fs.readFileSync(cfg,'utf8');assert.ok(forced.startsWith(original));
  assert.ok(forced.includes('@nClientDownloadEnableHTTP2PlatformLinux 0'));
  policy.applyLinuxSteamDownloadPolicy(policyRoot);assert.equal(fs.readFileSync(cfg,'utf8'),forced,'idempotent preference');
  policy.setLinuxSteamDownloadHttp1(false);policy.applyLinuxSteamDownloadPolicy(policyRoot);
  assert.equal(fs.readFileSync(cfg,'utf8'),original,'turning off removes only our block');
  const corruptCfg=original+'// BEGIN HMOSDECK DOWNLOAD CONNECTION\n';fs.writeFileSync(cfg,corruptCfg);
  assert.throws(()=>policy.applyLinuxSteamDownloadPolicy(policyRoot),/标记/);assert.equal(fs.readFileSync(cfg,'utf8'),corruptCfg);
  fs.unlinkSync(cfg);const external=path.join(temporary,'external.cfg');fs.writeFileSync(external,original);fs.symlinkSync(external,cfg);
  assert.throws(()=>policy.applyLinuxSteamDownloadPolicy(policyRoot),/安全/);assert.equal(fs.readFileSync(external,'utf8'),original);
  fs.unlinkSync(cfg);fs.writeFileSync(cfg,original);
  console.log('PASS: download preference preserves user config, applies once, reverses cleanly and refuses damaged/symlinked cfg');
  let directoryResolutions=0, resolverUnavailable=false;
  const paths=load('service/LinuxSteamPaths.ets',{});
  const storage=load('service/LinuxSteamDiagnosticData.ets',{
    './LinuxSteamPaths': paths,
    '@kit.AbilityKit':{}, '@kit.CoreFileKit':{fileIo:io,
      fileUri:{FileUri:class {constructor(uri){this.path=uri;}}},
      picker:{DocumentPickerMode:{DOWNLOAD:1},DocumentViewPicker:class {
        async save(options){assert.equal(options.pickerMode,1);directoryResolutions++;
          if(resolverUnavailable)throw new Error('directory unavailable');return [temporary];}
      }}},
    '@kit.BasicServicesKit':{}, './ConfigurationFiles':config,
    './ManagedSmokePayloadService':{ensureSmokeDirectory:dirs,readSmokeText:readText,
      writeSmokeText:(p,s)=>fs.writeFileSync(real(p),s)},
    './LinuxSteamReleaseRetention':retention,'./LinuxSteamLibraryLayout':{},'../steam/SteamLocalRules':{}
  });
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}),'');
  assert.equal(fs.existsSync(root),false,'status discovery does not create a Steam component directory');
  fs.mkdirSync(root);
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}),root);
  assert.equal(readText(pointer),root,'existing app download directory recovers its private pointer');
  const previousResolutions=directoryResolutions;
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}),root);
  assert.equal(directoryResolutions,previousResolutions,'cached directory avoids another system lookup');
  fs.unlinkSync(real(pointer));fs.unlinkSync(real('/data/storage/el2/base/files/steamohos-native/storage-location.txt'));fs.rmdirSync(root);
  fs.symlinkSync(temporary,root,'dir');
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}),'','symlinked roots are not adopted');
  assert.equal(readText(pointer),'');fs.unlinkSync(root);
  resolverUnavailable=true;
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}),'','unavailable lookup leaves status usable');
  resolverUnavailable=false;fs.mkdirSync(root);
  const api=load('service/LinuxSteamInstaller.ets',{
    './LinuxSteamPaths':paths,
    '@kit.AbilityKit':{},'@kit.CoreFileKit':{fileIo:io,statfs:{getFreeSize:async()=>free}},
    '@kit.BasicServicesKit':{zlib:{async decompressFile(file,where) {
      extractions++;
      if(path.basename(file)==='local-import.zip') {
        const target=path.join(where,'archives',sha(official)+'.zip');
        fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,official);return;
      }
      const group=path.basename(file).startsWith(sha(embedded))?'gnu-runtime':'official';
      for(const f of plan.files.filter(f=>f.archive===group)){
        const p=path.join(where,f.source);fs.mkdirSync(path.dirname(p),{recursive:true});
        fs.writeFileSync(p,corrupt && group==='official'?Buffer.from('bad'):content.get(f.path));
      }
    }}},'../model/LinuxSteamDownloadModels':model,
    './LinuxSteamLibraryService':{LinuxSteamLibraryService:{getInstance:()=>({externalRoot:()=>readText(pointer)})}},
    './LinuxSteamDiagnosticData':{prepareLinuxSteamExternalStorage:storage.prepareLinuxSteamExternalStorage,
      recoverLinuxSteamExternalStorage:storage.recoverLinuxSteamExternalStorage,
      selectLinuxSteamStorage:storage.selectLinuxSteamStorage, linuxSteamStorageLocation:storage.linuxSteamStorageLocation,
      prepareLinuxSteamClientData:async(_,__,id)=>{activates++;if(id)fs.writeFileSync(path.join(root,'active-version.txt'),id);}},
    './ConfigurationFiles':config,'./ManagedSmokePayloadService':{ensureSmokeDirectory:dirs,
      readSmokeText:p=>{try{return fs.readFileSync(real(p),'utf8');}catch{return '';}}},
    './LinuxSteamComponentDownload':{LinuxSteamComponentDownload:Transfer,LinuxSteamDownloadError:download.LinuxSteamDownloadError},
    './LinuxSteamReleaseRetention':retention
  });
  let fd;
  const context={resourceManager:{getRawFileContent:async name=>Buffer.from(JSON.stringify(name.endsWith('offline-package.json')?metadata:plan)),
    getRawFdSync:()=>({fd:fd=fs.openSync(embeddedPath,'r'),offset:0,length:embedded.length}),closeRawFdSync:()=>fs.closeSync(fd)}};
  const installer=api.LinuxSteamInstallService.getInstance();
  await assert.rejects(installer.install(context),e=>e.code==='SPACE');
  assert.equal(fetches,0);assert.equal(activates,0);assert.equal(installer.snapshot().phase,'failed');
  free=2**34;corrupt=true;
  await assert.rejects(installer.install(context),e=>e.code==='FILE_CHECKSUM');
  assert.equal(activates,0,'failure never switches active version');
  corrupt=false;
  const user=path.join(root,'client/.steam/steam/userdata/1');fs.mkdirSync(user,{recursive:true});
  fs.writeFileSync(path.join(user,'save'),'keep-user-data');
  await Promise.all([installer.install(context),installer.install(context)]);
  assert.equal(activates,1,'concurrent installs share one operation');
  assert.equal(installer.snapshot().phase,'ready');
  assert.equal(fs.readFileSync(path.join(user,'save'),'utf8'),'keep-user-data');
  assert.equal(fs.readFileSync(path.join(root,'active-version.txt'),'utf8'),plan.id);
  assert.equal(fs.readFileSync(path.join(root,'releases',plan.id,'.verified'),'utf8'),plan.id);
  assert.equal(fs.existsSync(path.join(root,'staging',plan.id)),false);
  fs.unlinkSync(real(pointer));
  fs.unlinkSync(real('/data/storage/el2/base/files/steamohos-native/storage-location.txt'));
  const beforeRediscovery={fetches,activates,extractions};
  await installer.initialize(context);
  assert.equal(installer.installed(),true,'lost private pointer rediscovers the verified existing installation');
  assert.equal(installer.snapshot().phase,'ready');
  assert.deepEqual({fetches,activates,extractions},beforeRediscovery,'status discovery does not download, unpack or activate');
  assert.equal(fs.readFileSync(path.join(user,'save'),'utf8'),'keep-user-data');
  await installer.install(context);assert.equal(activates,1,'ready installation is idempotent');
  const protectedFiles=new Map([
    [path.join(user,'save'),'keep-user-data'],
    [path.join(root,'library/steamapps/common/game/game.exe'),'user-downloaded-game'],
    [path.join(root,'library/steamapps/appmanifest_1.acf'),'game-install-record'],
    [path.join(root,'client/.steam/steam/config/loginusers.vdf'),'account'],
    [path.join(root,'releases',plan.id,'runtime/custom'),'unknown-runtime-file']
  ]);
  for(const [p,b] of protectedFiles){fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,b);}
  function assertProtected(){for(const [p,b] of protectedFiles)assert.equal(fs.readFileSync(p,'utf8'),b,p);}
  const networkBefore=fetches, unpackBefore=extractions;
  const invalid=path.join(temporary,'invalid.zip');fs.writeFileSync(invalid,Buffer.alloc(bundle.length,0));
  await assert.rejects(installer.importLocal(context,invalid),e=>e.code==='LOCAL_CHECKSUM');
  assert.equal(extractions,unpackBefore,'bad local bundle is never extracted');assert.equal(installer.installed(),true);assertProtected();
  metadata.id=sha('older-host-same-official-archives');
  await installer.importLocal(context,bundlePath);metadata.id=plan.id;
  assert.equal(installer.installed(),true,'a host-only update reuses the pinned official offline bundle');
  const legacyBundle=Buffer.from('previous-published-bundle-with-the-same-official-zips');
  const legacyPath=path.join(temporary,'legacy-offline.zip');fs.writeFileSync(legacyPath,legacyBundle);
  metadata.compatiblePackages=[{bytes:legacyBundle.length,sha256:sha(legacyBundle)}];
  await installer.importLocal(context,legacyPath);
  assert.equal(installer.installed(),true,'verified older publication accepted without changing client selection');
  assertProtected();assert.equal(fetches,networkBefore,'legacy import never falls back to network');
  fs.writeFileSync(legacyPath,Buffer.alloc(legacyBundle.length,0));
  const beforeCorruptLegacy=extractions;
  await assert.rejects(installer.importLocal(context,legacyPath),e=>e.code==='LOCAL_CHECKSUM');
  assert.equal(extractions,beforeCorruptLegacy,'same-size corruption of a legacy publication is rejected before extraction');
  metadata.compatiblePackages=Array(17).fill({bytes:bundle.length,sha256:sha(bundle)});
  await assert.rejects(installer.importLocal(context,bundlePath),e=>e.code==='LOCAL_VERSION');
  delete metadata.compatiblePackages;assertProtected();
  assert.equal(fetches,networkBefore,'bad import never uses network');assertProtected();
  const pauseLocal=value=>{if(value.message==='正在读取本地组件包')installer.pause();};
  installer.subscribe(pauseLocal);await installer.importLocal(context,bundlePath);installer.unsubscribe(pauseLocal);
  assert.equal(installer.snapshot().phase,'paused');assert.equal(installer.installed(),true);assertProtected();
  fs.writeFileSync(path.join(root,'client/.steam/steam/steamui/index.html'),'damaged');
  await installer.importLocal(context,bundlePath);
  assert.equal(fs.readFileSync(path.join(root,'client/.steam/steam/steamui/index.html'),'utf8'),'UI');
  assert.equal(fetches,networkBefore,'offline repair does not download');assert.equal(installer.installed(),true);assertProtected();
  await installer.resetComponents(context);
  assert.equal(installer.installed(),false);assert.equal(installer.snapshot().phase,'idle');assertProtected();
  assert.equal(fs.existsSync(path.join(root,'releases',plan.id,'runtime/steamrtarm64/steam')),false);
  assert.equal(fs.existsSync(path.join(root,'client/.steam/steam/steamui/index.html')),false);
  await installer.importLocal(context,bundlePath);
  assert.equal(installer.installed(),true,'reset then import restores verified marker');assertProtected();
  const pending=installer.install(context,true);
  await assert.rejects(installer.resetComponents(context),/暂停/);await pending;assertProtected();
  const publicRoot=readText(pointer).trim();
  await installer.selectLocation(context,'private');
  assert.equal(installer.location(),'private');assert.equal(installer.installed(),false);
  assert.equal(fs.existsSync(real(paths.LINUX_STEAM_PRIVATE_ROOT)),false,'selection creates no new Steam library');
  assert.equal(await storage.recoverLinuxSteamExternalStorage({}), '','explicit private selection cannot rediscover public games as active');
  assertProtected();
  await installer.selectLocation(context,'download');
  assert.equal(readText(pointer).trim(),publicRoot);assert.equal(installer.installed(),true);assertProtected();
  const oldId=plan.id, beforeUpgradeFetches=fetches;
  const upgradeLoader=Buffer.from('new-loader');content.set('release/runtime/ld-linux-aarch64.so.1',upgradeLoader);
  const loader=plan.files.find(f=>f.path.includes('ld-linux'));loader.bytes=upgradeLoader.length;loader.sha256=sha(upgradeLoader);
  plan.releaseManifest=JSON.stringify({schemaVersion:1,layout:'SteamLinuxARM64',executionModel:'native_aarch64',
    files:[...content].filter(([p])=>p.startsWith('release/')).map(([p,b])=>({path:p.substring(8),sha256:sha(b),bytes:b.length}))})+'\n';
  plan.id=sha(plan.releaseManifest);
  api.LinuxSteamInstallService.instance=null;
  const upgraded=api.LinuxSteamInstallService.getInstance();
  const uiPath=path.join(root,'client/.steam/steam/steamui/index.html');fs.writeFileSync(uiPath,'damaged');
  await assert.rejects(upgraded.ensureReady(context),e=>e.code==='UPDATE_REQUIRED');
  assert.equal(fs.readFileSync(path.join(root,'active-version.txt'),'utf8'),oldId,'failed adapter update keeps previous activation');
  assert.equal(fetches,beforeUpgradeFetches,'automatic adapter update never falls back to downloads');assertProtected();
  fs.writeFileSync(uiPath,'UI');await upgraded.ensureReady(context);
  assert.equal(fetches,beforeUpgradeFetches,'matching official client files are reused offline');
  assert.equal(fs.readFileSync(path.join(root,'active-version.txt'),'utf8'),plan.id);
  assert.equal(fs.readFileSync(path.join(root,'releases',plan.id,'runtime/ld-linux-aarch64.so.1'),'utf8'),'new-loader');assertProtected();
  console.log('PASS: HAP adapter upgrade reuses checksum-matched official files without network; damaged client blocks activation and keeps old install/game data');
  const clientDir=path.join(root,'client/.steam/steam/steamrtarm64');
  const outsideClient=path.join(temporary,'outside-client');fs.renameSync(clientDir,outsideClient);fs.symlinkSync(outsideClient,clientDir);
  await installer.resetComponents(context);assertProtected();
  assert.equal(fs.readFileSync(path.join(outsideClient,'icudtl.dat'),'utf8'),'ICU','reset never follows symlinked client parents');
  assertProtected();
  console.log('PASS: 206 resume, ignored Range, early response/dataEnd, interrupted transfer, checksum retry, 404 preserves partial, invalid/416 range, pause/restart, verified cache, production manifest and path/source boundaries');
  console.log('PASS: missing private pointer rediscovers verified components without download; empty, symlinked and unavailable download roots are handled safely');
  console.log('PASS: actual installer space preflight, corrupt unpacked file, activation only after success, concurrent install, cache/staging cleanup, existing userdata preserved and ready idempotence');
  console.log('PASS: wrong/corrupt offline bundles rejected before extraction, no-network repair, reset/reimport, maintenance locking; game files, install records, accounts, saves and unknown files preserved');
}
main().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>fs.rmSync(temporary,{recursive:true,force:true}));
