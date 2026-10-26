const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vpp-proton-test-'));
const base = home + '/steamohos-native', root = base + '/proton-probe', scratch = base + '/proton-component-install';
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const write = (name, text) => { fs.mkdirSync(path.dirname(name), {recursive:true}); fs.writeFileSync(name,text); };
let files = {}, manifest, hashes = 0, decompressions = 0, lowSpace = false, failActivation = false, badZip = false;
let remoteCatalog, catalogRevision = 1, downloadFailure = false, renameFailure = '';
const archives = new Map(), runtimeReleases = [];
const archive = Buffer.from('verified component archive');
const archivePath = home + '/embedded.zip'; write(archivePath, archive);
const makeManifest = version => {
  files = {};
  const names = ['package/proton','package/files/lib/wine/aarch64-unix/wine','package/files/bin-arm64/wineserver',
    'package/files/lib/wine/aarch64-unix/ntdll.so','package/files/lib/wine/i386-windows/ntdll.dll','hmosdeck','libproton_probe.so',
    'prefix/drive_c/proton-x86-locale.exe','prefix/drive_c/dxvk26/d3d11.dll','prefix/drive_c/dxvk26/dxgi.dll',
    'graphics/libproton_egl.so','graphics/libproton_vulkan.so','fonts/tahoma.ttf','locale/zh_CN.UTF-8/LC_CTYPE',
    'licenses/NOTICE.txt','audio-direct','manual-launch.version','runtime-modules.list','prefix-modules.list',
    ...Array.from({length:20}, (_,i) => `package/files/lib/${i}.dll`)];
  for (const name of names) files[name] = Buffer.from(`${version}:${name}`);
  const rows = names.map(name => ({path:name,bytes:files[name].length,sha256:hash(files[name]),mode:384}));
  manifest = {schemaVersion:1,id:hash(version),bytes:archive.length,sha256:hash(archive),
    installedBytes:rows.reduce((n,f)=>n+f.bytes,0),files:rows};
  const upstream = rows.filter(row => row.path.startsWith('package/'));
  const packed = Buffer.from('upstream-package-'+version);
  archives.set(hash(packed), {packed, files:{...files}});
  const release = {id:hash('runtime-'+version),name:version,architecture:'aarch64',adapterAbi:'hmosdeck-ge-wine11-arm64ec-v1',
    prefixAbi:'wine11-arm64ec-v1',installedBytes:upstream.reduce((n,f)=>n+f.bytes,0),files:upstream,
    archive:{url:'https://github.com/yifengling0/VintagePomeloPro/releases/download/runtime/'+version+'.zip',
      bytes:packed.length,sha256:hash(packed),unpackedBytes:manifest.installedBytes}};
  runtimeReleases.push(release);
  return release;
};
const catalog = () => ({schemaVersion:1,revision:catalogRevision,releases:[...runtimeReleases].reverse()});
const exists = name => fs.existsSync(name);
const deps = {
  '@kit.AbilityKit': {}, '@kit.CoreFileKit': {fileIo: {
    accessSync: exists, mkdirSync: (name, recursive) => fs.mkdirSync(name,{recursive}),
    lstatSync: name => {const s=fs.lstatSync(name);return {size:s.size,mtime:s.mtimeMs,isFile:()=>s.isFile(),isDirectory:()=>s.isDirectory(),isSymbolicLink:()=>s.isSymbolicLink()};},
    statSync: value => typeof value === 'number' ? fs.fstatSync(value) : fs.statSync(value),
    listFileSync: name=>fs.readdirSync(name), unlinkSync: name=>fs.unlinkSync(name),rmdirSync:name=>fs.rmdirSync(name),
    renameSync: (from,to) => {
      if (renameFailure && to.endsWith(renameFailure)) { renameFailure='';throw new Error('injected runtime failure'); }
      if (failActivation && from===scratch+'/new/graphics') { failActivation=false;throw new Error('injected activation failure'); }
      fs.renameSync(from,to);
    },
    OpenMode: {READ_ONLY:fs.constants.O_RDONLY,CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW},
    openSync: (name,flags) => ({fd:fs.openSync(name,flags)}),closeSync:file=>fs.closeSync(file.fd),fsyncSync:fd=>fs.fsyncSync(fd),
    readSync: (fd, buffer, options) => fs.readSync(fd,Buffer.from(buffer),0,options.length,options.offset),
    writeSync: (fd,buffer)=>fs.writeSync(fd,Buffer.from(buffer))
  },statfs:{getFreeSize:async()=>lowSpace?0:1e12}},
  '@kit.BasicServicesKit': {zlib:{decompressFile:async(_archive,destination)=>{
    decompressions++;
    const contents = _archive.endsWith('/runtime.zip') ? archives.get(hash(fs.readFileSync(_archive))).files : files;
    for (const [name,data] of Object.entries(contents)) write(destination+'/'+name,data);
    if(badZip) write(destination+'/hmosdeck','corrupt');
  }}},
  './ConfigurationFiles': {ConfigurationFiles: {
    ensure: name=>{fs.mkdirSync(name,{recursive:true});assert.ok(!fs.lstatSync(name).isSymbolicLink());},
    text: data=>Buffer.from(data).toString(),json:write,read:(name)=>exists(name)?fs.readFileSync(name):Buffer.alloc(0),
    fileHash:async name=>{hashes++;return hash(fs.readFileSync(name));}
  }},
  './ManagedSmokePayloadService': {ensureSmokeDirectory:()=>true,readSmokeText:name=>exists(name)?fs.readFileSync(name,'utf8'):''}
};
deps['@kit.NetworkKit'] = {http:{RequestMethod:{GET:'GET'},HttpDataType:{STRING:0},createHttp:()=>({
  request:async()=>({responseCode:200,result:JSON.stringify(remoteCatalog ?? catalog())}),destroy(){}
})}};
deps['./LinuxSteamComponentDownload'] = {LinuxSteamComponentDownload:class {
  pause(){} async fetch(descriptor,destination,progress) {
    if (downloadFailure) throw new Error('injected network failure');
    const data=archives.get(descriptor.sha256).packed;write(destination,data);progress(data.length);
  }
}};
function load(name) {
  const output = {};
  const text = fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/'+name+'.ets'),'utf8')
    .replaceAll('/data/storage/el2/base/files/steamohos-native',base);
  const code=ts.transpileModule(text,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
  vm.runInNewContext(code,{exports:output,setTimeout:fn=>setImmediate(fn),require:id=>{assert.ok(id in deps,id);return deps[id];}});
  return output;
}
const models=load('model/LinuxProtonRuntimeModels');
deps['../model/LinuxProtonRuntimeModels']=models;
const runtimeModule=load('service/LinuxProtonRuntimeService');
deps['./LinuxProtonRuntimeService']=runtimeModule;
const manager=()=>runtimeModule.LinuxProtonRuntimeService.getInstance();
const exportsObject={};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxGnuProtonComponent.ets'),'utf8')
  .replace('/data/storage/el2/base/files/steamohos-native',base);
const code=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
vm.runInNewContext(code,{exports:exportsObject,setTimeout:fn=>setImmediate(fn),require:id=>{assert.ok(id in deps,id);return deps[id];}});
const service=exportsObject.LinuxGnuProtonComponent;
const context={resourceManager:{getRawFileContent:async(name)=>Buffer.from(JSON.stringify(name.endsWith('runtime-catalog.json')?catalog():manifest)),
  getRawFdSync:()=>({fd:fs.openSync(archivePath,'r'),length:archive.length,offset:0}),closeRawFdSync:()=>{}}};
(async()=>{
  try {
    const v1=makeManifest('v1');
    for(const name of ['../game','home/save','prefix/system.reg','prefix/drive_c/game/save','package/../home/x','/absolute','package//x','package\\x'])
      assert.throws(()=>exportsObject.protonComponentGroup(name));
    const invalid={...manifest,files:[...manifest.files,manifest.files[0]]};
    assert.throws(()=>exportsObject.validateProtonComponent(invalid));
    write(root+'/prefix/user.reg','user registry');write(root+'/home/save','save');
    await Promise.all([service.ensureReady(context),service.ensureReady(context)]);
    assert.equal(decompressions,1);assert.ok(service.ready());
    assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'user registry');
    assert.equal(fs.readFileSync(root+'/home/save','utf8'),'save');
    const firstHashes=hashes; await service.ensureReady(context);
    assert.equal(hashes,firstHashes);assert.equal(decompressions,1,'repeat startup must reuse receipt');
    service.setSessionActive(true);await service.ensureReady(context);service.setSessionActive(false);
    const legacyInode=fs.statSync(root+'/package').ino, legacyExtracts=decompressions;
    fs.rmSync(base+'/proton-runtimes',{recursive:true});runtimeModule.LinuxProtonRuntimeService.instance=null;
    await service.ensureReady(context);assert.equal(decompressions,legacyExtracts);assert.equal(fs.statSync(root+'/package').ino,legacyInode);
    write(root+'/package/proton','damaged embedded');await service.ensureReady(context);
    assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v1:package/proton');
    const previous=fs.readFileSync(root+'/hmosdeck','utf8');const oldReceipt=fs.readFileSync(root+'/component-installed.json','utf8');
    const v2=makeManifest('v2');runtimeModule.LinuxProtonRuntimeService.instance=null;lowSpace=true;
    await assert.rejects(service.ensureReady(context),/空间不足/);lowSpace=false;
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    badZip=true; await assert.rejects(service.ensureReady(context),/文件校验失败/);badZip=false;
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    failActivation=true;await assert.rejects(service.ensureReady(context),/activation failure/);
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    assert.equal(fs.readFileSync(root+'/component-installed.json','utf8'),oldReceipt);
    assert.ok(!exists(scratch+'/transaction.json'));
    await service.ensureReady(context);assert.ok(fs.readFileSync(root+'/hmosdeck','utf8').startsWith('v2:'));
    // Simulate process death between moving an old group and activating a new one.
    fs.mkdirSync(scratch+'/old',{recursive:true});fs.renameSync(root+'/graphics',scratch+'/old/graphics');
    write(scratch+'/transaction.json',JSON.stringify({committed:false,previousReceipt:fs.readFileSync(root+'/component-installed.json','utf8'),changes:[{path:'graphics',hadOld:true}]}));
    await service.ensureReady(context);assert.ok(exists(root+'/graphics/libproton_egl.so'));
    write(root+'/hmosdeck','damaged');await service.ensureReady(context);
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),files.hmosdeck.toString());
    const probeResult=await service.verifyFreshPrefix(context,async()=>{
      assert.ok(!exists(root+'/prefix/user.reg'),'probe must use an empty prefix');
      assert.ok(exists(root+'/prefix/drive_c/proton-x86-locale.exe'),'helper must be delivered automatically');
      write(root+'/prefix/generated.reg','generated only');return 'initialized';
    });
    assert.equal(probeResult,'initialized');assert.ok(!exists(root+'/prefix/generated.reg'));
    await assert.rejects(service.verifyFreshPrefix(context,async()=>{throw new Error('probe child failed');}),/probe child failed/);
    assert.ok(!exists(root+'/prefix-before-install-test'));
    assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'user registry');
    assert.equal(fs.readFileSync(root+'/home/save','utf8'),'save');
    assert.ok(!exists(scratch+'/component.zip')&&!exists(scratch+'/new')&&!exists(scratch+'/old'));
    assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v1:package/proton','adapter upgrade must keep the selected upstream');
    const store=base+'/proton-runtimes', state=()=>JSON.parse(fs.readFileSync(store+'/selection.json'));
    assert.equal(state().activeId,v1.id);
    assert.ok(!exists(store+'/versions/'+v1.id+'/package'),'embedded adoption must not duplicate package');
    catalogRevision++;await manager().checkUpdates(context);
    const keep = new Map(['prefix/user.reg','home/save'].map(name=>[name,fs.readFileSync(root+'/'+name)]));
    const originalPackage=fs.statSync(root+'/package').ino;
    downloadFailure=true;await assert.rejects(manager().install(context,v2.id),/network failure/);downloadFailure=false;
    lowSpace=true;await assert.rejects(manager().install(context,v2.id),/空间不足/);lowSpace=false;
    assert.equal(fs.statSync(root+'/package').ino,originalPackage);
    const bad=home+'/bad.zip';write(bad,Buffer.alloc(v2.archive.bytes));
    await assert.rejects(manager().install(context,v2.id,bad),/压缩包校验失败/);
    assert.equal(state().activeId,v1.id);
    renameFailure='/proton-probe/package';
    await assert.rejects(manager().install(context,v2.id),/injected runtime failure/);
    assert.equal(state().activeId,v1.id);assert.equal(fs.statSync(root+'/package').ino,originalPackage);
    await manager().install(context,v2.id);assert.equal(state().activeId,v2.id);assert.equal(state().previousId,v1.id);
    const v2Package=fs.statSync(root+'/package').ino;
    write(store+'/versions/'+v2.id+'/runtime.json','invalid metadata');
    await assert.rejects(service.ensureReady(context));
    await manager().install(context,v2.id);assert.equal(fs.statSync(root+'/package').ino,v2Package,'metadata repair must reuse verified bytes');
    await manager().select(context,v1.id);assert.equal(fs.statSync(root+'/package').ino,originalPackage);
    await manager().select(context,v2.id);assert.equal(fs.statSync(root+'/package').ino,v2Package,'switch by rename, never copy');
    for(const [name,data] of keep) assert.deepEqual(fs.readFileSync(root+'/'+name),data);
    await service.ensureReady(context);assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v2:package/proton');
    service.setSessionActive(true);await assert.rejects(manager().select(context,v1.id),/请先退出/);service.setSessionActive(false);
    manager().beginSessionPreparation();await assert.rejects(manager().select(context,v1.id),/请先退出/);
    await service.ensureReady(context);manager().endSessionPreparation();
    const releaseGame=manager().holdGame();await assert.rejects(manager().select(context,v1.id),/请先退出/);
    await service.ensureReady(context);releaseGame();releaseGame();
    manager().beginAdapter();await assert.rejects(manager().select(context,v1.id),/请先退出/);manager().endAdapter();
    const maintenance=manager().checkUpdates(context);assert.throws(()=>manager().holdGame(),/组件正在维护/);await maintenance;
    // Each crash boundary of a rename transaction restores old state and bytes.
    for (const step of ['journal','old-moved','new-moved','state-saved','committed']) {
      const previous={...state()};const incoming=store+'/versions/'+v1.id+'/package';
      const outgoing=store+'/versions/'+v2.id+'/package';
      const journal={from:v2.id,to:v1.id,previous,committed:step==='committed',hadActive:true};
      write(store+'/activation.json',JSON.stringify(journal));
      if(step!=='journal') fs.renameSync(root+'/package',outgoing);
      if(['new-moved','state-saved','committed'].includes(step)) fs.renameSync(incoming,root+'/package');
      if(['state-saved','committed'].includes(step)) write(store+'/selection.json',JSON.stringify({activeId:v1.id,previousId:v2.id}));
      await service.ensureReady(context);
      assert.equal(state().activeId,step==='committed'?v1.id:v2.id);
      assert.ok(!exists(store+'/activation.json'));
      if(step==='committed') await manager().select(context,v2.id);
    }
    for(const step of ['journal','old-moved','new-moved','committed']) {
      const incoming=store+'/versions/'+v2.id+'/package', outgoing=store+'/staging/replaced-package';
      for(const file of v2.files) write(store+'/versions/'+v2.id+'/'+file.path,archives.get(v2.archive.sha256).files[file.path]);
      write(store+'/activation.json',JSON.stringify({from:v2.id,to:v2.id,previous:state(),committed:step==='committed',hadActive:true}));
      if(step!=='journal') fs.renameSync(root+'/package',outgoing);
      if(['new-moved','committed'].includes(step)) fs.renameSync(incoming,root+'/package');
      await service.ensureReady(context);assert.equal(state().activeId,v2.id);
      assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v2:package/proton');
      if(exists(incoming)) fs.rmSync(incoming,{recursive:true});
      assert.ok(!exists(outgoing));
    }
    fs.rmSync(root+'/package',{recursive:true});await manager().install(context,v2.id);
    assert.equal(state().activeId,v2.id);assert.ok(exists(root+'/package/proton'));
    // Same-ID repair and missing active files never lose the previous version.
    write(root+'/package/proton','damaged-runtime');
    await assert.rejects(service.ensureReady(context),/所选 Proton/);
    await manager().install(context,v2.id);assert.equal(state().previousId,v1.id);
    assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v2:package/proton');
    await manager().select(context,v1.id);write(root+'/package/proton','bad embedded');
    await assert.rejects(service.ensureReady(context),/所选 Proton/);
    await manager().install(context,v1.id);assert.equal(fs.readFileSync(root+'/package/proton','utf8'),'v1:package/proton');
    // Steam's original is read-only input; reject unknown architectures and links.
    const steam=home+'/SteamLinuxARM64', source=steam+'/library/steamapps/common/Proton v2';
    write(base+'/external-root.path',steam);
    for(const row of v2.files) write(source+'/'+row.path.substring(8),archives.get(v2.archive.sha256).files[row.path]);
    fs.mkdirSync(steam+'/library/steamapps/common/Proton x86_64');
    const found=await manager().discoverSteam(context);assert.equal(found.length,2);
    assert.equal(found.filter(row=>row.supported).length,1);
    const sourceHash=hash(fs.readFileSync(source+'/files/lib/wine/aarch64-unix/wine'));
    await assert.rejects(manager().importSteam(context,found.find(row=>!row.supported)),/不受支持/);
    const saved=store+'/versions/'+v2.id+'/package';fs.rmSync(saved,{recursive:true});
    const linked=source+'/files/lib/0.dll', original=fs.readFileSync(linked), outside=home+'/outside-file';
    write(outside,original);fs.unlinkSync(linked);fs.symlinkSync(outside,linked);
    await assert.rejects(manager().importSteam(context,found.find(row=>row.supported)));
    assert.equal(state().activeId,v1.id);fs.unlinkSync(linked);write(linked,original);
    await manager().importSteam(context,found.find(row=>row.supported));
    assert.equal(hash(fs.readFileSync(source+'/files/lib/wine/aarch64-unix/wine')),sourceHash);
    // Downloader/list errors preserve selection and existing users' data.
    const before=state().activeId;
    remoteCatalog={...catalog(),revision:1};await assert.rejects(manager().checkUpdates(context),/旧的/);
    remoteCatalog=catalog();remoteCatalog.releases=remoteCatalog.releases.map(row=>row.id===v1.id?{...row,files:row.files.map((f,i)=>i===0?{...f,sha256:'f'.repeat(64)}:f)}:row);
    await assert.rejects(manager().checkUpdates(context),/身份冲突/);
    remoteCatalog=null;assert.equal(state().activeId,before);
    assert.throws(()=>models.validateProtonRuntime({...v1,architecture:'x86_64'}),/不受支持/);
    assert.throws(()=>models.validateProtonRuntime({...v1,files:[...v1.files,{...v1.files[0],path:'package/../save'}]}));
    await assert.rejects(manager().remove(context,v2.id),/需保留/);
    const v3=makeManifest('v3');catalogRevision++;await manager().checkUpdates(context);await manager().install(context,v3.id);
    await manager().select(context,v2.id);await manager().select(context,v1.id);await manager().remove(context,v3.id);
    assert.ok(!exists(store+'/versions/'+v3.id));
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),'v2:hmosdeck','runtime updates must not replace adapter binaries');
    for(const [name,data] of keep) assert.deepEqual(fs.readFileSync(root+'/'+name),data);
    assert.ok(!exists(store+'/staging/runtime.zip') && !exists(store+'/staging/unpacked'));
    console.log('Runtime versions, adapter independence, download/import, rollback/crash recovery, Steam discovery, live-game guards and user data preservation: PASS');
    console.log('GNU Proton clean install, concurrency, cached startup, space failure, corruption, activation rollback, crash recovery, repair and user data preservation: PASS');
  } finally { fs.rmSync(home,{recursive:true,force:true}); }
})().catch(e=>{console.error(e);process.exitCode=1;});
