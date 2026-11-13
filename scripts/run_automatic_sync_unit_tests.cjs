#!/usr/bin/env node
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path');
const vm = require('node:vm'), crypto = require('node:crypto');
const ts = require(process.argv[3]), directory = path.resolve(process.argv[2]);
const app = require(path.join(directory, 'model/AppModels.js'));
const models = require(path.join(directory, 'model/ValueAddedModels.js'));
const syncModels = require(path.join(directory, 'model/AccountSyncModels.js'));
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/valueadded/AccountAutomaticSyncService.ets'), 'utf8');
const js = ts.transpileModule(source, {compilerOptions:{module:ts.ModuleKind.CommonJS, target:ts.ScriptTarget.ES2020}}).outputText;
const clone = x => JSON.parse(JSON.stringify(x));
const hash = x => crypto.createHash('sha256').update(x).digest('hex');
const A = 'a'.repeat(64), B = 'b'.repeat(64);
const payload = () => ({schemaVersion:1,themeMode:'system',uiSkin:'classic',consolePalette:'violet',
  consoleCardLayout:'focus3d',viewMode:'grid',useGlassEffect:true,desktopInputProfileId:'default'});
const profile = (id='custom',name='Custom') => ({id,name,schemaVersion:6,cursorSpeed:1,overlayOpacity:80,elements:[],gamepadMappings:[]});
function fixture(options={}) {
  let user=A, settings=payload(), offline=false, entitlement=true;
  const timers=new Map(), journal=new Map(), profiles=new Map(), remotes=options.cloud?.profiles || new Map(), heads=new Map(), conflicts=new Map();
  const calls={uploads:[],downloads:[],settingsUploads:0,settingsApplies:0,backups:[],configurations:0,resumes:0,capabilities:0,entitlements:0,journals:0};
  const configurationListeners=[];let configurationRevision=0,configurationHook=null;
  const configurationRepo={subscribeCloud:f=>configurationListeners.push(f),cloudRevision:()=>configurationRevision};
  let configurationSyncedAt=options.configurationSyncedAt??1000;
  const configuration={initialize:async()=>{},hasConflict:()=>false,usedBytes:()=>0,lastSyncedAt:()=>configurationSyncedAt,sync:async()=>{calls.configurations++;return configurationHook?configurationHook():'云端备份已更新';}};
  const accountListeners=[], profileListeners=[], themeListeners=[], entitlementListeners=[];
  let capabilities={profiles:true,preferences:true,...options.capabilities},recovery='',restoreFailure=false,restoreHook=null,refreshHook=null;
  let seq=0, beforeRead=null, beforeApply=null;
  const preferencesByOwner=options.cloud?.preferences || new Map();
  const remotePreferences=()=>preferencesByOwner.get(user)||null;
  for (const p of (options.profiles || [app.createDefaultInputProfile(),profile()])) profiles.set(p.id,clone(p));
  const remoteMap=()=>{if(!remotes.has(user))remotes.set(user,new Map());return remotes.get(user);};
  const envelope=(p,revision)=>({schemaVersion:1,deviceClass:'tablet',revision,payloadSha256:hash(JSON.stringify(models.cloudProfilePayload(p))),payload:models.cloudProfilePayload(p)});
  const check=()=>{if(offline)throw Error('OFFLINE');};
  const inputs={getProfiles:()=>Array.from(profiles.values()).map(clone),getProfile:id=>clone(profiles.get(id)),
    hasProfile:id=>profiles.has(id),hasDesktopProfile:id=>!id.startsWith('app-')&&profiles.has(id),
    subscribeChanges:f=>profileListeners.push(f)};
  const theme={getSettings:()=>clone(settings),subscribe:f=>themeListeners.push(f),update:async p=>{
    if(beforeApply)await beforeApply(); settings=clone(p);calls.settingsApplies++;themeListeners.forEach(f=>f(p));}};
  const api={getUserId:()=>user,getCapabilities:()=>capabilities,
    initialize:async()=>{},resumeAccount:async()=>{calls.resumes++;},hasRememberedAccount:()=>false,refreshCapabilities:async()=>{calls.capabilities++;if(refreshHook)refreshHook();},getPurchaseRecoveryStatus:()=>recovery,getPurchaseRecoveryCheckedAt:()=>Date.now(),
    subscribeEntitlement:f=>entitlementListeners.push(f),restorePurchases:async()=>{
      calls.restores=(calls.restores||0)+1;if(restoreHook)await restoreHook();
      entitlementListeners.forEach(f=>f());if(restoreFailure)throw Error('PROVIDER_UNAVAILABLE');
      recovery='ACK_PENDING';entitlement=true;return recovery;
    },
    subscribeAccount:f=>accountListeners.push(f),entitlement:async()=>{calls.entitlements++;check();return {active:entitlement,expiresAt:Date.now()+10000,serverTime:Date.now()};},
    payloadHash:async s=>hash(s),getConflict:id=>conflicts.get(id),setProfileBaseRevision:(id,r)=>heads.set(id,r),profileRevision:id=>heads.get(id)||0,
    listCloudProfiles:async()=>{check();if(beforeRead)await beforeRead();return Array.from(remoteMap().entries()).map(([id,e])=>({variant:id,name:e.payload.name,revision:e.revision}));},
    readCloudProfile:async id=>{check();return clone(remoteMap().get(id));},
    downloadProfile:async(id,copy,expected,valid)=>{check();const e=clone(remoteMap().get(id));calls.backups.push(clone(profiles.get(id)||null));
      if(beforeApply)await beforeApply();if(!valid())throw Error('ACCOUNT_CHANGED');
      const actual=profiles.has(id)?JSON.stringify(models.cloudProfilePayload(profiles.get(id))):'__absent__';
      if(actual!==expected)throw Error('PROFILE_LOCAL_CHANGED');
      profiles.set(id,clone(e.payload));conflicts.delete(id);heads.set(id,e.revision);calls.downloads.push(id);return e;},
    uploadProfile:async(p,resolve)=>{check();const current=remoteMap().get(p.id),base=resolve&&conflicts.has(p.id)?conflicts.get(p.id).revision:(heads.get(p.id)||0);
      if((current?.revision||0)!==base){conflicts.set(p.id,clone(current));calls.backups.push(clone(p),clone(current));return 'CONFLICT';}
      const next=envelope(p,base+1);remoteMap().set(p.id,next);heads.set(p.id,next.revision);conflicts.delete(p.id);calls.uploads.push({user,id:p.id,payload:clone(p)});return 'SAVED';},
    readPreferences:async()=>{check();return clone(remotePreferences());},
    uploadPreferences:async(p,base)=>{check();if((remotePreferences()?.revision||0)!==base)return {status:'CONFLICT',current:clone(remotePreferences())};
      preferencesByOwner.set(user,{schemaVersion:1,deviceClass:'tablet',revision:base+1,payloadSha256:hash(JSON.stringify(p)),payload:clone(p)});
      calls.settingsUploads++;return {status:'SAVED',current:clone(remotePreferences())};}
  };
  const store={getSync:(k,d)=>journal.get(k)||d,put:async(k,v)=>{calls.journals++;journal.set(k,v);},flush:async()=>{}};
  const imports={'@kit.AbilityKit':{},'@ohos.data.preferences':{default:{getPreferences:async()=>store}},
    '../../common/DeviceCapabilityPolicy':{DeviceCapabilityPolicy:{supportsValueAddedFeatures:()=>options.valueAddedSupported!==false}},
    '../../model/AppModels':app,'../../model/ValueAddedModels':models,'../../model/AccountSyncModels':syncModels,
    './ValueAddedService':{ValueAddedService:{getInstance:()=>api}},'../InputProfileService':{InputProfileService:{getInstance:()=>inputs}},
    '../ThemeSettingsService':{ThemeSettingsService:{getInstance:()=>theme}},
    '../UserConfigurationRepository':{UserConfigurationRepository:{getInstance:()=>configurationRepo}},
    './ConfigurationCloudSyncService':{ConfigurationCloudSyncService:{getInstance:()=>configuration}},
    '../AppSettingsStore':{AppSettingsStore:{getInstance:()=>({subscribeGlobalChanges:f=>themeListeners.push(f)})}}};
  const mod={exports:{}};
  vm.runInNewContext(js,{module:mod,exports:mod.exports,require:n=>{if(!imports[n])throw Error(n);return imports[n];},
    setTimeout:(f,ms)=>{timers.set(++seq,{f,ms});return seq;},clearTimeout:id=>timers.delete(id),console});
  const sync=mod.exports.AccountAutomaticSyncService.getInstance();
  return {sync,calls,profiles,journal,timers,api,cloud:{profiles:remotes,preferences:preferencesByOwner},settings:()=>settings,remoteMap,
    changeConfiguration:()=>{configurationRevision++;configurationListeners.forEach(f=>f());},configurationHook:f=>configurationHook=f,
    configurationTime:value=>configurationSyncedAt=value,
    fire:async()=>{const [id,job]=timers.entries().next().value;timers.delete(id);job.f();for(let i=0;i<100;i++){await new Promise(resolve=>setImmediate(resolve));if(!sync.busy&&!sync.preparing)break;}assert.equal(sync.busy,false);},
    initialize:async()=>sync.initialize({}),run:async()=>{sync.clearTimer();if(!capabilities.configuration)sync.forceConfigurationCheck=true;await sync.syncNow();},
    edit:p=>{profiles.set(p.id,clone(p));profileListeners.forEach(f=>f(p.id,false,'local'));},
    remove:id=>{profiles.delete(id);profileListeners.forEach(f=>f(id,true,'local'));},
    changeSettings:p=>{settings=clone(p);themeListeners.forEach(f=>f(settings));},
    setRemote:(p,r)=>remoteMap().set(p.id,envelope(p,r)),
    setPreferences:(p,r)=>{preferencesByOwner.set(user,{schemaVersion:1,deviceClass:'tablet',revision:r,payloadSha256:hash(JSON.stringify(p)),payload:clone(p)});},
    remotePreferences,
    user:async id=>{user=id;heads.clear();conflicts.clear();await sync.accountChanged(id);},
    offline:v=>offline=v,entitlement:v=>entitlement=v,recovery:v=>recovery=v,restoreFailure:v=>restoreFailure=v,
    restoreHook:f=>restoreHook=f,refreshHook:f=>refreshHook=f,capabilities:v=>capabilities=v,
    entitlementEvent:()=>entitlementListeners.forEach(f=>f()),beforeRead:f=>beforeRead=f,beforeApply:f=>beforeApply=f};
}
let count=0;
async function test(name,run){await run();count++;console.log('PASS '+name);}
(async()=>{
 await test('hidden account features tolerate game-window background and foreground without account calls',async()=>{
  const f=fixture({valueAddedSupported:false});
  f.api.getUserId=()=>{throw Error('FEATURE_HIDDEN');};
  await f.initialize();const before=JSON.stringify(f.calls);
  for(let i=0;i<3;i++){assert.doesNotThrow(()=>f.sync.setForeground(false));assert.doesNotThrow(()=>f.sync.setForeground(true));}
  await f.sync.syncNow();assert.equal(JSON.stringify(f.calls),before);assert.equal(f.timers.size,0);assert.equal(f.journal.size,0);
 });
 await test('first login uploads automatically; steady state sends no redundant writes',async()=>{
  const f=fixture();await f.initialize();await f.run();assert.equal(f.calls.uploads.length,2);assert.equal(f.calls.settingsUploads,1);
  const last=f.sync.lastSyncedAt();await f.run();assert.equal(f.calls.uploads.length,2);assert.equal(f.calls.settingsUploads,1);assert.ok(last>0);
 });
 await test('same device reinstall restores its cloud backup before uploading factory defaults',async()=>{
  const original=fixture({profiles:[...app.createBuiltInInputProfiles(),profile()]});await original.initialize();await original.run();
  const saved={...payload(),themeMode:'dark',uiSkin:'console',desktopInputProfileId:'custom'};
  original.changeSettings(saved);original.edit({...profile(),cursorSpeed:1.75});await original.run();
  const remoteBefore=clone(original.remotePreferences()),profilesBefore=clone(Array.from(original.remoteMap()));
  const reinstalled=fixture({cloud:original.cloud,profiles:app.createBuiltInInputProfiles()});
  assert.equal(reinstalled.journal.size,0);await reinstalled.initialize();await reinstalled.run();
  assert.deepEqual(reinstalled.settings(),saved);assert.equal(reinstalled.profiles.get('custom').cursorSpeed,1.75);
  assert.equal(reinstalled.calls.settingsUploads,0);assert.equal(reinstalled.calls.uploads.length,0);
  assert.deepEqual(original.remotePreferences(),remoteBefore);assert.deepEqual(Array.from(original.remoteMap()),profilesBefore);
  assert.equal(reinstalled.sync.status(),'已恢复云端备份',JSON.stringify(reinstalled.sync.itemStates()));assert.ok(reinstalled.sync.lastSyncedAt()>0);
  await reinstalled.run();assert.equal(reinstalled.calls.settingsUploads,0);assert.equal(reinstalled.calls.uploads.length,0);
 });
 await test('reinstall while offline retains cloud backup and restores after reconnecting',async()=>{
  const original=fixture({profiles:[...app.createBuiltInInputProfiles(),profile()]});await original.initialize();await original.run();original.changeSettings({...payload(),viewMode:'large'});await original.run();
  const saved=clone(original.remotePreferences());
  const reinstalled=fixture({cloud:original.cloud,profiles:app.createBuiltInInputProfiles()});reinstalled.offline(true);
  await reinstalled.initialize();await reinstalled.run();assert.equal(reinstalled.settings().viewMode,'grid');
  assert.equal(reinstalled.calls.settingsUploads,0);assert.equal(reinstalled.sync.lastSyncedAt(),0);
  assert.deepEqual(original.remotePreferences(),saved);reinstalled.offline(false);await reinstalled.run();
  assert.equal(reinstalled.settings().viewMode,'large');assert.equal(reinstalled.calls.settingsUploads,0);
 });
 await test('settings changed after reinstall restoration update the same backup',async()=>{
  const original=fixture({profiles:[...app.createBuiltInInputProfiles(),profile()]});await original.initialize();await original.run();original.changeSettings({...payload(),themeMode:'dark'});await original.run();
  const revision=original.remotePreferences().revision;
  const reinstalled=fixture({cloud:original.cloud,profiles:app.createBuiltInInputProfiles()});await reinstalled.initialize();await reinstalled.run();
  reinstalled.changeSettings({...reinstalled.settings(),viewMode:'compact'});await reinstalled.run();
  assert.equal(original.remotePreferences().revision,revision+1);assert.equal(original.remotePreferences().payload.themeMode,'dark');
  assert.equal(original.remotePreferences().payload.viewMode,'compact');assert.equal(reinstalled.calls.settingsUploads,1);
 });
 await test('fresh built-in IDs permit restore but edited local controls remain a conflict',async()=>{
  const original=fixture({profiles:app.createBuiltInInputProfiles()});await original.initialize();await original.run();
  original.edit({...original.profiles.get('default'),cursorSpeed:1.5});await original.run();
  const reinstalled=fixture({cloud:original.cloud,profiles:app.createBuiltInInputProfiles()});await reinstalled.initialize();await reinstalled.run();
  assert.equal(reinstalled.profiles.get('default').cursorSpeed,1.5);assert.equal(reinstalled.calls.uploads.length,0);
  const edited=app.createBuiltInInputProfiles();edited.find(p=>p.id==='default').elements[0].x=0.3;
  const withLocalEdits=fixture({cloud:original.cloud,profiles:edited});await withLocalEdits.initialize();await withLocalEdits.run();
  assert.equal(withLocalEdits.profiles.get('default').elements[0].x,0.3);assert.ok(withLocalEdits.api.getConflict('default'));
  assert.equal(original.remoteMap().get('default').payload.cursorSpeed,1.5);
 });
 await test('local edits coalesce into one pending foreground job',async()=>{
  const f=fixture();await f.initialize();await f.run();for(let i=0;i<5;i++)f.edit(profile('custom','Edit '+i));
  assert.equal(f.timers.size,1);await f.run();assert.equal(f.remoteMap().get('custom').payload.name,'Edit 4');assert.equal(f.calls.uploads.length,3);
 });
 await test('remote changes apply only when local baseline is unchanged',async()=>{
  const f=fixture();await f.initialize();await f.run();f.setRemote(profile('custom','Remote'),2);await f.run();
  assert.equal(f.profiles.get('custom').name,'Remote');assert.equal(f.calls.downloads.length,1);
 });
 await test('both sides changed preserves branches and waits for explicit conflict resolution',async()=>{
  const f=fixture();await f.initialize();await f.run();f.edit(profile('custom','Local'));f.setRemote(profile('custom','Remote'),2);await f.run();
  assert.equal(f.profiles.get('custom').name,'Local');assert.equal(f.remoteMap().get('custom').payload.name,'Remote');assert.ok(f.api.getConflict('custom'));
  assert.match(f.sync.status(),/冲突/);await f.sync.resolveProfile('custom',false);assert.equal(f.remoteMap().get('custom').payload.name,'Local');
 });
 await test('offline queues edits and recovers without user uploading',async()=>{
  const f=fixture();await f.initialize();await f.run();f.offline(true);f.edit(profile('custom','Offline'));await f.run();assert.match(f.sync.status(),/重试/);
  assert.equal(Array.from(f.timers.values())[0].ms,60000);f.offline(false);await f.run();assert.equal(f.remoteMap().get('custom').payload.name,'Offline');
 });
 await test('account toggles persist and disabled families make no transfers',async()=>{
  const f=fixture();await f.initialize();await f.sync.setEnabled('profiles',false);await f.sync.setEnabled('preferences',false);await f.run();
  assert.equal(f.calls.uploads.length,0);assert.equal(f.calls.settingsUploads,0);assert.match(f.sync.status(),/关闭/);
  assert.equal(JSON.parse(f.journal.get('state_v1')).accounts[0].profilesEnabled,false);
 });
 await test('unsynced runtime changes do not claim another account preferences; allowed changes do',async()=>{
  const f=fixture();await f.initialize();await f.run();await f.user(B);await f.run();
  f.changeSettings({...f.settings(),wineLanguage:'en_US',outputScale:75});await f.run();
  assert.equal(f.calls.settingsUploads,1);assert.equal(f.sync.data.preferencesOwner,A);
  f.changeSettings({...f.settings(),viewMode:'compact'});await f.run();
  assert.equal(f.calls.settingsUploads,2);assert.equal(f.sync.data.preferencesOwner,B);
 });
 await test('account switch never uploads previous account profiles or preferences',async()=>{
  const f=fixture();await f.initialize();await f.run();await f.user(B);await f.run();assert.equal(f.calls.uploads.filter(x=>x.user===B).length,0);
  assert.match(f.sync.status(),/其他账号/);assert.equal(f.calls.settingsUploads,1);
 });
 await test('sign-out during suspended remote read prevents all subsequent writes',async()=>{
  const f=fixture();await f.initialize();let enter,release;const started=new Promise(r=>enter=r),paused=new Promise(r=>release=r);
  f.beforeRead(async()=>{enter();await paused;});const running=f.run();await started;await f.user('');release();await running;
  assert.equal(f.calls.uploads.length,0);assert.equal(f.calls.settingsUploads,0);assert.equal(f.sync.status(),'等待登录');assert.equal(f.timers.size,0);
 });
 await test('foreground loss prevents remote apply and retries after return',async()=>{
  const f=fixture();await f.initialize();await f.run();f.setRemote(profile('custom','Remote'),2);
  let enter,release;const started=new Promise(r=>enter=r),paused=new Promise(r=>release=r);
  f.beforeApply(async()=>{enter();await paused;});const running=f.run();await started;f.sync.setForeground(false);release();await running;
  assert.equal(f.profiles.get('custom').name,'Custom');assert.equal(f.timers.size,0);f.beforeApply(null);f.sync.setForeground(true);await f.run();assert.equal(f.profiles.get('custom').name,'Remote');
 });
 await test('local deletion is remembered and cloud copy does not resurrect',async()=>{
  const f=fixture();await f.initialize();await f.run();f.remove('custom');await f.run();assert.equal(f.profiles.has('custom'),false);
  assert.ok(f.remoteMap().has('custom'));assert.ok(JSON.parse(f.journal.get('state_v1')).accounts[0].removedProfiles.includes('custom'));
 });
 await test('local edit while downloading is preserved and retried as conflict',async()=>{
  const f=fixture();await f.initialize();await f.run();f.setRemote(profile('custom','Remote'),2);
  f.beforeApply(async()=>{f.beforeApply(null);f.edit(profile('custom','New local'));});await f.run();assert.equal(f.profiles.get('custom').name,'New local');
  assert.match(f.sync.status(),/新修改/);await f.run();assert.ok(f.api.getConflict('custom'));
 });
 await test('missing default profile postpones settings atomically without changing baseline or cloud',async()=>{
  const f=fixture({capabilities:{profiles:false}});await f.initialize();const p={...payload(),themeMode:'dark',desktopInputProfileId:'missing'};f.setPreferences(p,1);
  await f.run();assert.equal(f.settings().themeMode,'system');assert.equal(f.calls.settingsApplies,0);assert.equal(f.calls.settingsUploads,0);
  assert.equal(f.sync.data.accounts[0].preferences,null);assert.match(f.sync.status(),/等待/);
 });
 await test('input scheme downloads before its default selection and settings apply',async()=>{
  const f=fixture();await f.initialize();f.setRemote(profile('desktop-custom','Desktop'),1);
  f.setPreferences({...payload(),themeMode:'dark',desktopInputProfileId:'desktop-custom'},1);await f.run();
  assert.equal(f.settings().desktopInputProfileId,'desktop-custom');assert.equal(f.settings().themeMode,'dark');assert.equal(f.calls.settingsApplies,1);
 });
 await test('equal preferences clear an earlier conflict; both changed do not overwrite',async()=>{
  const f=fixture();await f.initialize();await f.run();f.changeSettings({...payload(),themeMode:'light'});f.setPreferences({...payload(),themeMode:'dark'},2);
  await f.run();assert.equal(f.sync.hasPreferencesConflict(),true);assert.equal(f.settings().themeMode,'light');
  f.setPreferences({...payload(),themeMode:'light'},3);await f.run();assert.equal(f.sync.hasPreferencesConflict(),false);
 });
 await test('old cloud built-in cannot downgrade current shipped input layout',async()=>{
  const shipped=app.createBuiltInInputProfiles()[0],f=fixture({profiles:[shipped]});await f.initialize();
  f.setRemote({...shipped,builtInRevision:Math.max(0,shipped.builtInRevision-1),cursorSpeed:0.5},1);await f.run();
  assert.equal(f.profiles.get(shipped.id).cursorSpeed,shipped.cursorSpeed);assert.equal(f.calls.downloads.length,0);assert.ok(f.api.getConflict(shipped.id));
 });
 await test('partial capability and expired membership never claim full success',async()=>{
  const f=fixture({capabilities:{preferences:false}});await f.initialize();await f.run();assert.match(f.sync.status(),/部分/);assert.equal(f.sync.lastSyncedAt(),0);
  assert.ok(f.sync.itemStates().some(x=>x.id==='preferences'&&x.state==='云同步暂未开放'));
  f.entitlement(false);f.edit(profile('custom','No member'));await f.run();assert.match(f.sync.status(),/VIP/);assert.equal(f.calls.uploads.length,2);
 });
 await test('corrupt owner/baseline journal is rejected before network writes',async()=>{
  const f=fixture();f.journal.set('state_v1',JSON.stringify({schemaVersion:1,lastOwner:'evil',preferencesOwner:'',profileOwners:[],accounts:[]}));
  await f.initialize();assert.match(f.sync.status(),/不可用/);assert.equal(f.calls.uploads.length,0);
  const good={schemaVersion:1,lastOwner:A,preferencesOwner:A,profileOwners:[],accounts:[{userId:A,profilesEnabled:true,preferencesEnabled:true,
    profiles:[],preferences:null,removedProfiles:[],lastSyncedAt:0}]};syncModels.validateAutomaticSyncData(good);
  assert.throws(()=>syncModels.validateAutomaticSyncData({...good,accounts:[...good.accounts,...good.accounts]}),/INVALID_SYNC_STATE/);
  assert.throws(()=>syncModels.validateAutomaticSyncData({...good,profileOwners:[{id:'x',userId:'unknown'}]}),/INVALID_SYNC_STATE/);
 });
 await test('purchase restoration opens backup without another payment and avoids a self-triggered fast loop',async()=>{
  const f=fixture();await f.initialize();f.capabilities({commerce:true,profiles:true,preferences:true});f.recovery('PROVIDER_UNAVAILABLE');f.entitlement(false);
  await f.run();assert.equal(f.calls.restores,1);assert.equal(f.calls.settingsUploads,1);assert.equal(f.sync.status(),'已同步');
  assert.equal(f.timers.size,0);
  await f.run();assert.equal(f.calls.restores,2);assert.equal(f.calls.settingsUploads,1);assert.equal(f.timers.size,0);
 });
 await test('failed legacy receipts do not block a separately verified active entitlement or cause a fast notification loop',async()=>{
  const f=fixture();await f.initialize();f.capabilities({commerce:true,profiles:true,preferences:true});f.recovery('PROVIDER_UNAVAILABLE');f.restoreFailure(true);
  await f.run();assert.equal(f.calls.settingsUploads,1);assert.equal(f.timers.size,0);
  f.restoreFailure(false);f.entitlementEvent();assert.equal(Array.from(f.timers.values())[0].ms,0);await f.run();assert.equal(f.calls.settingsUploads,1);
 });
 await test('changed deployment capabilities are reread before automatic backup',async()=>{
  const f=fixture({capabilities:{profiles:false,preferences:false}});await f.initialize();await f.run();assert.equal(f.calls.settingsUploads,0);
  f.refreshHook(()=>f.capabilities({profiles:true,preferences:true}));await f.run();assert.equal(f.calls.settingsUploads,1);assert.equal(f.sync.status(),'已同步');
 });
 await test('account switching during subscription restoration never writes the old account backup',async()=>{
  const f=fixture();await f.initialize();f.capabilities({commerce:true,profiles:true,preferences:true});f.recovery('PROVIDER_UNAVAILABLE');
  f.restoreHook(()=>f.user(B));await f.run();assert.equal(f.calls.settingsUploads,0);assert.equal(f.calls.uploads.length,0);
 });
 await test('normal saves visibly queue and upload through the timer without visiting the sync page',async()=>{
  const f=fixture();await f.initialize();await f.run();
  f.edit({...profile(),cursorSpeed:1.83});assert.match(f.sync.status(),/本地修改/);
  assert.equal(f.sync.itemStates().find(i=>i.id==='custom').state,'等待备份');assert.equal(f.timers.size,1);
  await f.fire();assert.equal(f.remoteMap().get('custom').payload.cursorSpeed,1.83);
  f.changeSettings({...f.settings(),themeMode:'dark'});await f.fire();assert.equal(f.remotePreferences().payload.themeMode,'dark');
 });
 await test('expired session is silently resumed before the next scheduled backup, without a page switch',async()=>{
  const f=fixture();await f.initialize();await f.run();await f.user('');
  f.api.hasRememberedAccount=()=>true;
  f.api.resumeAccount=async()=>{await f.user(A);};
  f.edit({...profile(),cursorSpeed:1.9});await f.fire();assert.equal(f.remoteMap().get('custom').payload.cursorSpeed,1.9);
 });
 await test('configuration edits do not repeatedly restore an already checked purchase',async()=>{
  const f=fixture();await f.initialize();f.capabilities({commerce:true,profiles:true,preferences:true});f.recovery('RESTORED');
  f.api.getPurchaseRecoveryCheckedAt=()=>Date.now()-60001;await f.run();assert.equal(f.calls.restores||0,0);
  f.recovery('ACK_PENDING');f.entitlementEvent();await f.fire();assert.equal(f.calls.restores,1);
 });
 await test('failed legacy receipt still cannot grant backup when the official entitlement is inactive',async()=>{
  const f=fixture();await f.initialize();f.capabilities({commerce:true,profiles:true,preferences:true});f.recovery('ORDER_BINDING_MISMATCH');
  f.restoreFailure(true);f.entitlement(false);f.restoreHook(()=>f.entitlement(false));
  // This fixture's restore mock must leave official inactive state intact on failure.
  f.api.restorePurchases=async()=>{throw Error('ORDER_BINDING_MISMATCH');};
  await f.run();assert.equal(f.calls.settingsUploads,0);assert.equal(f.calls.uploads.length,0);assert.match(f.sync.status(),/VIP/);
 });
 await test('unified backup stays fully idle after success, including no journal or entitlement calls',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();assert.equal(f.timers.size,0);
  const calls=JSON.stringify(f.calls);for(let i=0;i<12;i++)await f.run();assert.equal(JSON.stringify(f.calls),calls);assert.equal(f.timers.size,0);
 });
 await test('unified local saves coalesce and go idle after the last configuration revision',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();for(let i=0;i<9;i++)f.changeConfiguration();
  assert.equal(f.timers.size,1);await f.fire();assert.equal(f.calls.configurations,2);assert.equal(f.timers.size,0);
 });
 await test('unified transient errors after prior success still retry with increasing delay',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();f.changeConfiguration();f.configurationHook(()=>{throw Error('OFFLINE');});
  await f.fire();assert.equal([...f.timers.values()][0].ms,60000);await f.fire();assert.equal([...f.timers.values()][0].ms,300000);
  f.configurationHook(null);await f.fire();assert.equal(f.timers.size,0);assert.equal(f.sync.status(),'云端备份已更新');
 });
 await test('idle foreground is local only; manual checks and disabled backup retain their behavior',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();const idle=JSON.stringify(f.calls);
  for(let i=0;i<15;i++){f.sync.setForeground(false);f.sync.setForeground(true);}assert.equal(f.timers.size,0);assert.equal(JSON.stringify(f.calls),idle);
  f.sync.requestSync(0,true);await f.fire();assert.equal(f.calls.configurations,2);await f.sync.setEnabled('configuration',false);const calls=JSON.stringify(f.calls);await f.fire();assert.equal(JSON.stringify(f.calls),calls);assert.equal(f.timers.size,0);
 });
 await test('duplicate initialization and login observations share one startup job',async()=>{
  const f=fixture({capabilities:{configuration:true}});
  f.api.initialize=async()=>{await f.sync.accountChanged(A);};
  await f.initialize();assert.equal(f.timers.size,1);assert.equal(f.calls.journals,1);
  await Promise.all([f.sync.accountChanged(A),f.sync.accountChanged(A)]);await f.fire();
  assert.equal(f.calls.configurations,1);assert.equal(f.timers.size,0);
 });
 await test('continuous local saves use a two-second debounce and one cloud job',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();
  for(let i=0;i<20;i++)f.changeConfiguration();
  assert.equal(f.timers.size,1);assert.equal([...f.timers.values()][0].ms,2000);
  await f.fire();assert.equal(f.calls.configurations,2);assert.equal(f.timers.size,0);
 });
 await test('startup login and purchase-recovery events wait for initialization and share one transfer',async()=>{
  const f=fixture({capabilities:{configuration:true}});let enter,release;
  const entered=new Promise(r=>enter=r),gate=new Promise(r=>release=r);
  f.api.initialize=async()=>{await f.sync.accountChanged(A);f.entitlementEvent();enter();await gate;f.entitlementEvent();};
  const initialization=f.initialize();await entered;
  assert.equal(f.timers.size,0,'startup cannot transfer while account restoration is incomplete');
  release();await initialization;assert.equal(f.timers.size,1);await f.fire();
  assert.equal(f.calls.configurations,1);assert.equal(f.timers.size,0);
 });
 await test('failed work retries at one, five and fifteen minutes then waits for a new event',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();
  f.configurationHook(()=>{throw Error('OFFLINE');});f.changeConfiguration();await f.fire();
  for(const delay of [60000,300000,900000]){assert.equal([...f.timers.values()][0].ms,delay);await f.fire();}
  assert.equal(f.timers.size,0);assert.equal(f.sync.retryExhausted,true);assert.equal(f.calls.configurations,5);
  f.sync.setForeground(false);f.sync.setForeground(true);assert.equal(f.timers.size,0);
  f.changeConfiguration();assert.equal([...f.timers.values()][0].ms,2000);await f.fire();assert.equal([...f.timers.values()][0].ms,60000);
  f.configurationHook(null);await f.fire();assert.equal(f.timers.size,0);assert.equal(f.sync.status(),'云端备份已更新');
 });
 await test('background edits wait for foreground and do not reset a pending failure budget',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();await f.run();
  f.sync.setForeground(false);f.changeConfiguration();assert.equal(f.timers.size,0);
  f.sync.setForeground(true);await f.fire();assert.equal(f.calls.configurations,2);
  f.configurationHook(()=>{throw Error('OFFLINE');});f.changeConfiguration();await f.fire();
  f.sync.setForeground(false);assert.equal(f.timers.size,0);f.sync.setForeground(true);
  assert.equal(f.sync.retryAttempt,1);assert.ok([...f.timers.values()][0].ms>59000);
  await f.fire();assert.equal([...f.timers.values()][0].ms,300000);
 });
 await test('foreground resumed before an interrupted transfer completes continues pending work once',async()=>{
  const f=fixture();await f.initialize();let entered,release;
  const started=new Promise(r=>entered=r),blocked=new Promise(r=>release=r);
  f.beforeRead(async()=>{entered();await blocked;});
  const running=f.run();await started;f.sync.setForeground(false);f.sync.setForeground(true);
  assert.equal(f.timers.size,0);release();await running;
  assert.equal(f.timers.size,1);f.beforeRead(null);await f.fire();assert.equal(f.timers.size,0);
  assert.equal(f.sync.status(),'已同步');
 });
 await test('unified modifications during transfer queue one more job and paused membership stops polling',async()=>{
  const f=fixture({capabilities:{configuration:true}});await f.initialize();let once=true;f.configurationHook(()=>{if(once){once=false;f.changeConfiguration();return '检测到本地修改，等待备份';}return '云端备份已更新';});
  await f.run();assert.equal(f.timers.size,1);await f.fire();assert.equal(f.calls.configurations,2);assert.equal(f.timers.size,0);
  f.entitlement(false);f.sync.requestSync(0,true);await f.fire();assert.match(f.sync.status(),/VIP/);assert.equal(f.timers.size,0);
 });
 await test('legacy migration transport also stays idle without configuration changes',async()=>{
  const f=fixture();await f.initialize();await f.run();const before=JSON.stringify(f.calls);await f.sync.syncNow();assert.equal(JSON.stringify(f.calls),before);assert.equal(f.timers.size,0);
 });
 await test('the unified backup time never claims an old partial profile upload was a complete backup',async()=>{
  const f=fixture({capabilities:{configuration:true},configurationSyncedAt:0});await f.initialize();f.sync.data.accounts[0].lastSyncedAt=9999;
  f.entitlement(false);await f.run();assert.equal(f.sync.lastSyncedAt(),0);assert.equal(f.timers.size,0);
  f.configurationTime(3210);assert.equal(f.sync.lastSyncedAt(),3210);
  f.capabilities({profiles:true,preferences:true,configuration:false});assert.equal(f.sync.lastSyncedAt(),9999);
 });
 console.log(`Automatic account sync: ${count} scenarios PASS (host mocks, no real account/network)`);
})().catch(error=>{console.error(error);process.exitCode=1;});
