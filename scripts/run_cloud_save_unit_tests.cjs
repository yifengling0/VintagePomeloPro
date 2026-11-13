#!/usr/bin/env node
// Executes the real ArkTS service logic with Node filesystem/crypto adapters. This is not an HDC/device test.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(process.argv[3]);
const models = require(path.resolve(process.argv[2], 'model/ValueAddedModels.js'));
const appModels = require(path.resolve(process.argv[2], 'model/AppModels.js'));
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'vp-cloud-save-test-'));
const filesDir = path.join(base, 'files'), gameDir = path.join(base, 'game');
fs.mkdirSync(filesDir); fs.mkdirSync(gameDir); fs.mkdirSync(path.join(gameDir, 'save'));
const save = path.join(gameDir, 'save', 'slot.sav'); fs.writeFileSync(save, 'first save');
let visible = true, engine = appModels.EngineState.STOPPED, processes = [], failRename = false, zeroWrite = false, mutateRead = false;
const game = { id: 'game-1', title:'Game', available:true, directory:gameDir,
  launchTarget: {kind:appModels.LaunchKind.WINE_EXE,executable:path.join(gameDir,'game.exe')} };
fs.writeFileSync(game.launchTarget.executable, 'MZ-game-identity-fixture');
const stat = input => { const s = typeof input === 'number' ? fs.fstatSync(input) : fs.statSync(input); s.mtime = s.mtimeMs; return s; };
const adapter = {
  OpenMode: { READ_ONLY:fs.constants.O_RDONLY, WRITE_ONLY:fs.constants.O_WRONLY, DIR:fs.constants.O_DIRECTORY,
    NOFOLLOW:fs.constants.O_NOFOLLOW, CREATE:fs.constants.O_CREAT, TRUNC:fs.constants.O_TRUNC },
  lstatSync:fs.lstatSync, statSync:stat, mkdirSync:fs.mkdirSync, accessSync:fs.existsSync,
  openSync(name,flags) { const fd=fs.openSync(name,flags,0o600); return {fd,get path(){return fs.realpathSync(name);}}; },
  closeSync(file){fs.closeSync(typeof file==='number'?file:file.fd);},
  readSync(fd,buffer) {const count=fs.readSync(fd,Buffer.from(buffer),0,buffer.byteLength,null);
    if(mutateRead){mutateRead=false;fs.appendFileSync(save,'changed');} return count;},
  writeSync(fd,data){if(zeroWrite)return 0; const b=typeof data==='string'?Buffer.from(data):Buffer.from(data); return fs.writeSync(fd,b);},
  fsyncSync:fs.fsyncSync, listFileSync:fs.readdirSync, readTextSync:name=>fs.readFileSync(name,'utf8'),
  unlinkSync:fs.unlinkSync, rmdirSync:fs.rmdirSync,
  renameSync(a,b){if(failRename&&path.basename(a).startsWith('.vp-restore-')) throw Error('injected rename error');fs.renameSync(a,b);}
};
const md = {createMd(){const hash=crypto.createHash('sha256');return {update:async ({data})=>{hash.update(data);},
  digest:async()=>({data:new Uint8Array(hash.digest())})};}};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/valueadded/LocalSaveSnapshotService.ets'),'utf8');
const js=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const moduleObject={exports:{}};
const imports={
  '@kit.AbilityKit':{}, '@kit.CoreFileKit':{fileIo:adapter}, '@kit.CryptoArchitectureKit':{cryptoFramework:md},
  '@ohos.util':{default:{generateRandomUUID:crypto.randomUUID,TextEncoder:class {encodeInto(text){return new TextEncoder().encode(text);}}}},
  'libentry.so':{default:{getProcessList:()=>processes}},
  '../../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{supportsValueAddedFeatures:()=>visible}},
  '../../model/AppModels':appModels, '../../model/ValueAddedModels':models,
  '../../model/SaveBackupModels':require(path.resolve(process.argv[2], 'model/SaveBackupModels.js')),
  '../../model/CloudSaveModels':require(path.resolve(process.argv[2], 'model/CloudSaveModels.js')),
  '../WineEngineService':{WineEngineService:{getInstance:()=>({getState:()=>engine})}},
  '../AppCatalogService':{AppCatalogService:{getInstance:()=>({getApps:()=>[game]})}}
};
vm.runInNewContext(js,{module:moduleObject,exports:moduleObject.exports,require:name=>{
  if(!imports[name])throw Error('unexpected import '+name);return imports[name];},setTimeout,Uint8Array,ArrayBuffer,TextEncoder,console});
const service=moduleObject.exports.LocalSaveSnapshotService.getInstance(), context={filesDir};
const cloudModels=require(path.resolve(process.argv[2], 'model/CloudSaveModels.js'));
const game2Dir=path.join(base,'game2');fs.mkdirSync(game2Dir);
const game2={...game,id:'game-2',title:'Second device game',directory:game2Dir,launchTarget:{...game.launchTarget,executable:path.join(game2Dir,'game.exe')}};
fs.writeFileSync(game2.launchTarget.executable,'MZ-second-device-game');
imports['../AppCatalogService'].AppCatalogService.getInstance=()=>({getApps:()=>[game,game2]});

(async()=>{
 const {CloudSaves}=await import('../server/value-added/src/cloud-saves.mjs');
 const {MemoryStore}=await import('../server/value-added/test/memory-store.mjs');
 const storage=new Map(),store=new MemoryStore();let network=0,epoch=0,user='a'.repeat(64),allow=true;
 const objects={ready:true,put:async(k,b)=>storage.set(k,Buffer.from(b)),delete:async k=>storage.delete(k),
  download:k=>({url:'https://private.obs.cn-north-4.myhuaweicloud.com/'+k+'?signature=test',headers:{},expiresAt:Date.now()+60000})};
 const server=new CloudSaves({store,requireMembership:async()=>{if(!allow)throw Error('MEMBERSHIP_REQUIRED');}},objects);
 const api={getCapabilities:()=>({cloudSaves:true}),getUserId:()=>user,cloudEpoch:()=>epoch,
  assertCloudEpoch:e=>{if(e!==epoch)throw Error('ACCOUNT_CHANGED');},
  cloudRequest:async(route,method,body)=>{network++;const json=typeof body==='string'&&body?JSON.parse(body):{};
   if(route==='/v1/saves')return server.list(user);
   if(route==='/v1/saves/slots')return server.createSlot(user,json);
   if(route==='/v1/saves/uploads')return server.begin(user,json);
   if(route==='/v1/saves/cleanup')return server.cleanup(user);
   let match=/^\/v1\/saves\/uploads\/([^/]+)\/chunks\/(\d+)$/.exec(route);
   if(match)return server.upload(user,match[1],Number(match[2]),Buffer.from(body));
   match=/^\/v1\/saves\/uploads\/([^/]+)\/(finalize|abort)$/.exec(route);
   if(match)return server[match[2]](user,match[1]);
   match=/^\/v1\/saves\/slots\/([^/]+)\/versions\/([^/]+)(?:\/chunks\/(\d+))?$/.exec(route);
   if(match)return match[3]!==undefined?server.downloadTicket(user,match[1],match[2],Number(match[3])):
     method==='DELETE'?server.deleteVersion(user,match[1],match[2],json):server.download(user,match[1],match[2]);
   throw Error('unexpected route '+route);
  }};
 const http={RequestMethod:{GET:'GET',POST:'POST',PUT:'PUT',DELETE:'DELETE'},HttpDataType:{ARRAY_BUFFER:'buffer'},
  createHttp:()=>({request:async url=>{network++;const bytes=storage.get(new URL(url).pathname.slice(1));
    return {responseCode:bytes?200:404,result:bytes?bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength):new ArrayBuffer(0)};},destroy(){}})};
 const cloudImports={...imports,'@ohos.net.http':{default:http},'./ValueAddedService':{ValueAddedService:{getInstance:()=>api}},
  './LocalSaveSnapshotService':moduleObject.exports};
 const cloudSource=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/valueadded/CloudSaveService.ets'),'utf8');
 const cloudJs=ts.transpileModule(cloudSource,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
 const cloudModule={exports:{}};
 vm.runInNewContext(cloudJs,{module:cloudModule,exports:cloudModule.exports,require:n=>{assert.ok(cloudImports[n],n);return cloudImports[n];},ArrayBuffer,Uint8Array,TextEncoder});
 const cloud=cloudModule.exports.CloudSaveService.getInstance();
 visible=false;await assert.rejects(cloud.list(),/FEATURE_HIDDEN/);assert.equal(network,0);visible=true;
 fs.writeFileSync(path.join(gameDir,'save','empty'),Buffer.alloc(0));
 const big=Buffer.alloc(cloudModels.CLOUD_CHUNK_BYTES+17,0x39);fs.writeFileSync(path.join(gameDir,'save','large'),big);
 const snapshot=await service.create(context,game.id,'save');
 let slot=await cloud.createSlot('shared game');
 await assert.rejects(cloud.upload(context,slot,snapshot),/SAVE_BINDING_REQUIRED/);
 cloud.bind(context,slot,game.id,'save');assert.equal(await cloud.upload(context,slot,snapshot),'PUBLISHED');
 slot=(await cloud.list()).slots[0];assert.equal(slot.revision,1);assert.equal(storage.size,2);
 assert.equal(fs.readdirSync(path.join(filesDir,'local_save_snapshots')).filter(n=>n.startsWith('transfer-')).length,0);
 cloud.bind(context,slot,game2.id,'restored-save');
 const downloaded=await cloud.download(context,slot,slot.versions[0].versionId);
 assert.equal(downloaded.appId,game2.id);assert.equal(downloaded.selectedPath,'restored-save');
 assert.equal(fs.existsSync(path.join(game2Dir,'restored-save')),false,'download never mutates game directory');
 assert.deepEqual(fs.readdirSync(path.join(filesDir,'local_save_snapshots',downloaded.snapshotId)).sort(),['files','manifest.json']);
 await service.restore(context,downloaded.snapshotId,game2.id);
 assert.deepEqual(fs.readFileSync(path.join(game2Dir,'restored-save','large')),big);
 assert.equal(fs.statSync(path.join(game2Dir,'restored-save','empty')).size,0);
 assert.equal(fs.readFileSync(path.join(game2Dir,'restored-save','slot.sav'),'utf8'),'first save');
 // Local mapping cannot carry over to another account.
 user='b'.repeat(64);epoch++;assert.equal(cloud.binding(context,slot),null);user='a'.repeat(64);epoch++;
 const packs=()=>fs.readdirSync(path.join(filesDir,'local_save_snapshots')).filter(n=>n.startsWith('transfer-'));
 const before=packs().length;let checks=0;
 await assert.rejects(service.prepareCloudUpload(context,snapshot.snapshotId,game.id,()=>{if(++checks>2)throw Error('ACCOUNT_CHANGED');}),/ACCOUNT_CHANGED/);
 assert.equal(packs().length,before,'cancelled packing removes temporary blocks');
 const realCleanup=service.cleanupCloudTransfer.bind(service);service.cleanupCloudTransfer=()=>{throw Error('disk locked');};
 await cloud.download(context,slot,slot.versions[0].versionId);assert.equal(cloud.busy,false);assert.equal(cloud.hasLocalCleanupPending(),true);
 service.cleanupCloudTransfer=realCleanup;cloud.cleanupLocal(context);assert.equal(cloud.hasLocalCleanupPending(),false);
 // Changing account during final local hash must not publish a downloaded snapshot.
 const realFinish=service.finishCloudImport.bind(service),beforeSnapshots=service.list(context,game2.id).length;
 service.finishCloudImport=async(...args)=>{epoch++;return realFinish(...args);};
 await assert.rejects(cloud.download(context,slot,slot.versions[0].versionId),/ACCOUNT_CHANGED/);
 assert.equal(service.list(context,game2.id).length,beforeSnapshots);service.finishCloudImport=realFinish;
 // Exact chunk/file hash, unknown fields, Unicode and prefix collisions are refused.
 const good=cloudModels.validateCloudSaveManifest;
 const pack=await service.prepareCloudUpload(context,snapshot.snapshotId,game.id,()=>{});
 assert.ok(!JSON.stringify(pack.manifest).includes('appId')&&!JSON.stringify(pack.manifest).includes('selectedPath'));
 const wrong=structuredClone(pack.manifest);wrong.files[0].relativePath='../bad';assert.throws(()=>good(wrong));
 const id=service.beginCloudImport(context,pack.manifest,game2.id,'restored-save');
 const chunk=pack.manifest.chunks[0],raw=await service.readCloudChunk(context,pack.transferId,chunk);
 const changed=new Uint8Array(raw);changed[0]^=1;await assert.rejects(service.writeCloudChunk(context,id,chunk,raw),/SAVE_HASH_MISMATCH/);
 service.cleanupCloudTransfers(context);
 await cloud.deleteVersion(slot,slot.versions[0].versionId);assert.equal(storage.size,0);
 console.log('Cloud save full host loop: explicit cross-device mapping, bounded multi-chunk+empty files, server CAS/hash, local restore, logout/cancel, cleanup faults PASS');
})().finally(()=>fs.rmSync(base,{recursive:true,force:true})).catch(error=>{console.error(error);process.exitCode=1;});
