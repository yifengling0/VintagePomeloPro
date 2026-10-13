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
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'vp-save-test-'));
const filesDir = path.join(base, 'files'), gameDir = path.join(base, 'game');
fs.mkdirSync(filesDir); fs.mkdirSync(gameDir); fs.mkdirSync(path.join(gameDir, 'save'));
const save = path.join(gameDir, 'save', 'slot.sav'); fs.writeFileSync(save, 'first save');
let visible = true, engine = appModels.EngineState.STOPPED, processes = [], failRename = false, zeroWrite = false, mutateRead = false;
const game = { id: 'game-1', title:'Game', available:true, directory:gameDir,
  launchTarget: {kind:appModels.LaunchKind.WINE_EXE,executable:path.join(gameDir,'game.exe')} };
fs.writeFileSync(game.launchTarget.executable, 'MZ-game-identity-fixture');
const stat = input => { const s = typeof input === 'number' ? fs.fstatSync(input) : fs.statSync(input); s.mtime = s.mtimeMs; return s; };
const fdTargets = new Map();
const adapter = {
  OpenMode: { READ_ONLY:fs.constants.O_RDONLY, WRITE_ONLY:fs.constants.O_WRONLY, DIR:fs.constants.O_DIRECTORY,
    NOFOLLOW:fs.constants.O_NOFOLLOW, CREATE:fs.constants.O_CREAT, TRUNC:fs.constants.O_TRUNC },
  lstatSync:fs.lstatSync, statSync:stat, mkdirSync:fs.mkdirSync, accessSync:fs.existsSync,
  openSync(name,flags) { const fd=fs.openSync(name,flags,0o600); fdTargets.set(fd,name); return {fd,get path(){return fs.realpathSync(name);}}; },
  closeSync(file){fs.closeSync(typeof file==='number'?file:file.fd);},
  readSync(fd,buffer) {const count=fs.readSync(fd,Buffer.from(buffer),0,buffer.byteLength,null);
    if(mutateRead && fdTargets.get(fd)===save){mutateRead=false;fs.appendFileSync(save,'changed');} return count;},
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
(async()=>{
  visible=false; await assert.rejects(service.create(context,game.id,'save'),/FEATURE_HIDDEN/); visible=true;
  engine=appModels.EngineState.READY;await assert.rejects(service.create(context,game.id,'save'),/SAVE_ENGINE_RUNNING/);engine=appModels.EngineState.STOPPED;
  processes=[{state:'running',path:'unknown-child'}];await assert.rejects(service.create(context,game.id,'save'),/SAVE_ENGINE_RUNNING/);processes=[];
  for(const invalid of ['../secret','/tmp','save/../save'])await assert.rejects(service.create(context,game.id,invalid),/UNSAFE_SAVE_PATH/);
  fs.symlinkSync(path.join(base,'files'),path.join(gameDir,'link'));await assert.rejects(service.create(context,game.id,'link'),/UNSAFE_SAVE_PATH/);
  const snap=await service.create(context,game.id,'save');assert.equal(snap.files.length,1);assert.equal(service.list(context,game.id).length,1);
  assert.equal(snap.files[0].sha256,crypto.createHash('sha256').update('first save').digest('hex'));
  assert.ok(!JSON.stringify(snap).includes(base),'manifest must not contain absolute roots');
  fs.writeFileSync(save,'second save');await service.restore(context,snap.snapshotId,game.id);assert.equal(fs.readFileSync(save,'utf8'),'first save');
  assert.ok(service.list(context,game.id).length>=2,'pre-restore snapshot retained');
  fs.writeFileSync(save,'third save');failRename=true;
  await assert.rejects(service.restore(context,snap.snapshotId,game.id),/injected rename error/);failRename=false;
  assert.equal(fs.readFileSync(save,'utf8'),'third save','failed swap rolled back');
  fs.unlinkSync(save); await service.restore(context,snap.snapshotId,game.id);
  assert.equal(fs.readFileSync(save,'utf8'),'first save','empty current save directory can be restored');
  fs.rmSync(path.join(gameDir,'save'),{recursive:true}); await service.restore(context,snap.snapshotId,game.id);
  assert.equal(fs.readFileSync(save,'utf8'),'first save','missing current save directory can be restored');
  const before=service.list(context,game.id).length;fs.writeFileSync(save,'changed before write failure');zeroWrite=true;
  await assert.rejects(service.create(context,game.id,'save'),/SAVE_WRITE_FAILED/);zeroWrite=false;
  assert.equal(service.list(context,game.id).length,before,'short-write snapshot not published');
  mutateRead=true;await assert.rejects(service.create(context,game.id,'save'),/SAVE_CHANGED/);
  fs.writeFileSync(path.join(gameDir,'save','SLOT.SAV'),'collision');await assert.rejects(service.create(context,game.id,'save'),/SAVE_PATH_COLLISION/);
  fs.unlinkSync(path.join(gameDir,'save','SLOT.SAV'));
  fs.writeFileSync(path.join(gameDir,'.steam-game.json'),JSON.stringify({source:'steam',appId:123,name:'Steam game'}));
  assert.equal(service.eligibleGames().length,0,'Steam marker excludes DOWNLOAD source too');fs.unlinkSync(path.join(gameDir,'.steam-game.json'));
  const snapshotFile=path.join(filesDir,'local_save_snapshots',snap.snapshotId,'files','slot.sav');fs.writeFileSync(snapshotFile,'corrupt');
  const current=fs.readFileSync(save,'utf8');await assert.rejects(service.restore(context,snap.snapshotId,game.id),/SAVE_HASH_MISMATCH/);
  assert.equal(fs.readFileSync(save,'utf8'),current,'hash failure never replaces current save');
  // Conservative automatic detection and explicit per-game opt-in.
  fs.writeFileSync(snapshotFile,'first save'); fs.writeFileSync(save,'first save');
  assert.equal(service.backupState(context,game.id).supported,true);
  assert.equal(service.backupState(context,game.id).enabled,false,'default is off, never opt in from a folder name');
  fs.mkdirSync(path.join(gameDir,'savegame')); fs.writeFileSync(path.join(gameDir,'savegame','another.sav'),'another');
  assert.equal(service.backupState(context,game.id).supported,false,'ambiguous locations disable backup');
  await assert.rejects(service.setBackupEnabled(context,game.id,true),/SAVE_NOT_RECOGNIZED/);
  fs.rmSync(path.join(gameDir,'savegame'),{recursive:true});
  fs.renameSync(path.join(gameDir,'save'),path.join(gameDir,'存档'));
  fs.renameSync(path.join(gameDir,'存档','slot.sav'),path.join(gameDir,'存档','slot.dat'));
  assert.equal(service.backupState(context,game.id).supported,true,'Chinese dedicated directory supports save data');
  fs.renameSync(path.join(gameDir,'存档','slot.dat'),path.join(gameDir,'存档','slot.sav'));
  fs.renameSync(path.join(gameDir,'存档'),path.join(gameDir,'save'));
  for(let i=0;i<100;i++)fs.mkdirSync(path.join(gameDir,'directory-'+i));
  assert.equal(service.backupState(context,game.id).supported,false,'incomplete discovery must not claim one trustworthy candidate');
  for(let i=0;i<100;i++)fs.rmdirSync(path.join(gameDir,'directory-'+i));

  await service.setBackupEnabled(context,game.id,true);
  assert.equal(service.backupState(context,game.id).enabled,true);
  const summary=service.summaries(context)[0];assert.equal(summary.title,'Game');
  await service.create(context,game.id,'save');
  const count=service.list(context,game.id).length;await service.runAutomatic(context);
  assert.equal(service.list(context,game.id).length,count,'unchanged content reuses an intact version');
  fs.writeFileSync(save,'automatic save');await service.runAutomatic(context);
  assert.equal(service.list(context,game.id).length,count+1,'opted-in changed save is backed up');
  const preview=await service.preview(context,game.id,'save',snap.snapshotId);
  assert.equal(preview.changed,1);
  fs.writeFileSync(save,'changed during confirmation');
  await assert.rejects(service.restore(context,snap.snapshotId,game.id,preview),/SAVE_CHANGED/);
  assert.equal(fs.readFileSync(save,'utf8'),'changed during confirmation');
  game.available=false;service.reconcileCatalog(context);
  assert.equal(service.summaries(context)[0].installed,false,'removed games stay in backup history');
  assert.equal(service.summaries(context)[0].count,count+1);
  game.available=true;assert.equal(service.backupState(context,game.id).enabled,false,'reimport never silently re-enables backup');
  fs.writeFileSync(game.launchTarget.executable,'MZ-another-game');
  await assert.rejects(service.restore(context,snap.snapshotId,game.id),/SAVE_IDENTITY_CHANGED/);
  game.steam={appId:123};assert.equal(service.eligibleGames().length,0,'new imported Steam cards excluded');delete game.steam;
  fs.writeFileSync(game.launchTarget.executable,'MZ-game-identity-fixture');
  await service.setBackupEnabled(context,game.id,true);
  fs.writeFileSync(game.launchTarget.executable,'MZ-updated-game');await service.runAutomatic(context);
  assert.equal(service.backupState(context,game.id).enabled,false,'changed executable pauses automatic backups');
  console.log('Automatic backup: unique detection, opt-in, unchanged reuse, confirmation race, archived history, reimport, Steam exclusion, identity guard PASS');
  console.log('Local snapshots: hidden/running guard, traversal/symlink/collision, hash, stable scans, backup, rollback, short writes PASS');
})().finally(()=>fs.rmSync(base,{recursive:true,force:true})).catch(error=>{console.error(error);process.exitCode=1;});
