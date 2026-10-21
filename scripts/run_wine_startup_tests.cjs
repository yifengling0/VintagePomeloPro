// Execute the complete production service with deterministic SDKs and clock.
// No test changes the user's prefix, runtime, or device files.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const ts = require(process.argv[2]);
const text = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/WineEngineService.ets'), 'utf8');
const compiled = ts.transpileModule(text, {compilerOptions:{target:ts.ScriptTarget.ES2020, module:ts.ModuleKind.CommonJS}}).outputText;
const State = {STOPPED:'stopped', PREPARING:'preparing', SWITCHING:'switching', READY:'ready', ERROR:'error'};
function fixture() {
  let now=1000, next=0, launches=0, stops=0;
  const timers=new Map();
  let snapshot={generation:0,active:false,state:'idle',phase:''};
  const settings={wineLanguage:'zh_CN',defaultDisplayMode:'desktop'};
  const wm={presentationMode:'desktop',applyModePolicy(){},setStartupForegroundGuard(){},prepareOutputForOrientation(){}};
  const napi={
    getWineStartupState:()=>JSON.stringify(snapshot),getProcessList:()=>[],
    setHostGraphicsBackend:()=>true,startServer:()=>true,
    launchClient(){++launches;snapshot={generation:snapshot.generation+1,active:true,state:'starting',phase:'wineboot'};return 0;},
    stopAll(){++stops;snapshot.active=false;snapshot.state='cancelled';}
  };
  function schedule(fn,ms,repeat) {const id=++next;timers.set(id,{fn,ms,repeat,at:now+ms});return id;}
  const context={exports:{},Date:{now:()=>now},Uint8Array,ArrayBuffer,JSON,Math,Set,Map,Promise,
    setTimeout:(f,t)=>schedule(f,t,false),setInterval:(f,t)=>schedule(f,t,true),
    clearTimeout:id=>timers.delete(id),clearInterval:id=>timers.delete(id),
    require(name) {
      if(name==='libentry.so')return {default:napi};
      if(name==='@kit.CoreFileKit')return {fileIo:{accessSync:()=>false}};
      if(name.includes('LogService'))return {hilog:{info(){},warn(){},error(){}}};
      if(name.includes('AppModels'))return {EngineState:State,WineLanguage:{CHINESE:'zh_CN'},
        WinePresentationMode:{DESKTOP:'desktop',MANAGED_WINDOWS:'managed'},
        ScreenOrientationSetting:{SENSOR_LANDSCAPE:'landscape'},canTransitionEngineState:()=>true};
      if(name.includes('WineUpgradeGuard'))return {WineUpgradeGuard:class {needsReset(){return false;}}};
      if(name.includes('AppSettingsStore'))return {AppSettingsStore:{getInstance:()=>({getGlobalSettings:()=>settings})}};
      if(name.includes('WineWindowManager'))return {WineWindowManager:{getInstance:()=>wm}};
      return {};
    }
  };
  vm.runInNewContext(compiled,context);
  const s=new context.exports.WineEngineService();
  s.context={};s.selectedD3DBackend=()=> 'dxvk_legacy';s.getBundledRuntimeVersion=()=>new Uint8Array([1]);
  s.ensureRuntimeInstalled=async()=>false;s.ensureProductDirectCapability=async()=>true;
  s.syncManagedSmokePayload=()=>true;s.writeVersionMarker=()=>true;
  async function flush(){for(let n=0;n<20;n++)await Promise.resolve();}
  async function advance(ms){
    await flush();
    const until=now+ms;
    while(true){let key,first=Infinity;for(const [id,t]of timers)if(t.at<first){first=t.at;key=id;}
      if(first>until)break; now=first;const t=timers.get(key);if(t.repeat)t.at+=t.ms;else timers.delete(key);t.fn();await flush();}
    now=until;await flush();
  }
  const start=()=>s.startSession('desktop','desktop','/games','reuse');
  return {s,napi,start,flush,advance,timers,stats:()=>({launches,stops}),snapshot:()=>snapshot};
}
function deferred(){let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};}
const watchdog=setTimeout(()=>{console.error("Unresolved startup test");process.exit(1);},15000);
(async()=>{
  let count=0;
  async function test(name,run){await run();++count;console.log('PASS '+name);}
  await test('terminal before waiter/native callback is recovered from retained state',async()=>{
    const f=fixture();f.napi.launchClient=()=>{Object.assign(f.snapshot(),{generation:1,active:false,state:'ready',phase:'explorer'});return 0;};
    assert.equal(await f.start(),true);assert.equal(f.s.state,State.READY);assert.equal(f.timers.size,0);
  });
  await test('missing ready callback recovers by polling; duplicate phases do not renew clock',async()=>{
    const f=fixture();const p=f.start();await f.flush();await f.advance(1000);
    const elapsed=f.s.getPhaseElapsedMs();for(let n=0;n<40;n++)f.s.handleNativeState('state:starting:wineboot');
    assert.equal(f.s.getPhaseElapsedMs(),elapsed);
    Object.assign(f.snapshot(),{state:'ready',active:false});await f.advance(250);
    assert.equal(await p,true);assert.equal(f.stats().launches,1);
  });
  await test('concurrent first launches share extraction; repeated callers cannot prolong timeout',async()=>{
    const f=fixture(),d=deferred();let installs=0;
    f.s.ensureRuntimeInstalled=()=>{++installs;return d.promise;};
    const p=f.start();await f.advance(119000);const q=f.start();
    assert.equal(installs,1);await f.advance(1000);assert.equal(await p,false);assert.equal(await q,false);
    assert.equal(f.s.state,State.ERROR);d.resolve(false);await f.flush();assert.equal(f.stats().launches,0);
    assert.equal(f.stats().stops,1);await f.advance(300);assert.equal(f.timers.size,0);
  });
  await test('native startup cannot wait indefinitely or revive from late ready/phase events',async()=>{
    const f=fixture(),p=f.start();await f.flush();await f.advance(125000);
    assert.equal(await p,false);assert.equal(f.s.state,State.ERROR);assert.equal(f.stats().stops,1);
    for(const raw of ['state:ready','evt:desktop-ready','phase:graphics','state:starting:wineboot'])f.s.handleNativeState(raw);
    assert.equal(f.s.state,State.ERROR);
  });
  await test('stop during prerequisite cancels callers and prevents late native launch',async()=>{
    const f=fixture(),d=deferred();f.s.ensureRuntimeInstalled=()=>d.promise;
    const p=f.start(),stop=f.s.stopSession();await f.advance(300);await stop;
    assert.equal(await p,false);d.resolve(false);await f.flush();assert.equal(f.stats().launches,0);
    assert.equal(f.s.state,State.STOPPED);assert.equal(f.timers.size,0);
  });
  await test('native immediate rejection is an explicit failure, not an orphan waiter',async()=>{
    const f=fixture();f.napi.launchClient=()=>-1;assert.equal(await f.start(),false);
    assert.equal(f.s.state,State.ERROR);assert.match(f.s.message,/未被接受/);
  });
  await test('native early-return failure without any callback releases all callers',async()=>{
    const f=fixture(),p=f.start(),q=f.start();await f.flush();Object.assign(f.snapshot(),{state:'failed',phase:'coordinator',active:false});
    await f.advance(250);assert.equal(await p,false);assert.equal(await q,false);assert.equal(f.stats().launches,1);
  });
  await test('timeout drains then retry starts a single new coordinator',async()=>{
    const f=fixture(),p=f.start();await f.flush();await f.advance(125000);assert.equal(await p,false);await f.advance(300);
    const q=f.start();await f.advance(300);await f.flush();assert.equal(f.stats().launches,2);
    Object.assign(f.snapshot(),{state:'ready',active:false});await f.advance(250);assert.equal(await q,true);
  });
  await test('graphics capability promise is bounded and late completion cannot start Wine',async()=>{
    const f=fixture(),d=deferred();f.s.ensureProductDirectCapability=()=>d.promise;const p=f.start();await f.flush();
    await f.advance(45000);assert.equal(await p,false);d.resolve(true);await f.flush();assert.equal(f.stats().launches,0);
  });
  await test('late old failure/drain callbacks cannot abort a replacement session',async()=>{
    const f=fixture(),p=f.start();await f.flush();
    f.s.handleNativeState('state:failed:wineboot');assert.equal(f.s.state,State.PREPARING);
    Object.assign(f.snapshot(),{state:'ready',active:false});await f.advance(250);assert.equal(await p,true);
    f.s.handleNativeState('state:failed:wineserver');f.s.handleNativeState('state:stopped');assert.equal(f.s.state,State.READY);
  });
  await test('old native coordinator must drain before retry, and drain timeout releases the blocker',async()=>{
    const f=fixture(),p=f.start();await f.flush();
    f.napi.stopAll=()=>{f.snapshot().state='cancelled';};
    const stop=f.s.stopSession();const checked=assert.rejects(stop,/旧 Wine/);
    await f.advance(35000);await checked;assert.equal(await p,false);assert.equal(f.s.state,State.ERROR);
    assert.equal(f.snapshot().active,true);assert.equal(f.stats().launches,1);
  });
  await test('failed restart always clears the shared promise so retry remains available',async()=>{
    const f=fixture();f.s.stopSession=async()=>{throw Error('drain failed');};
    await assert.rejects(f.s.restart('desktop','desktop','/games'),/drain failed/);assert.equal(f.s.restartPromise,null);
  });
  await test('fresh diagnostic binds prefix, socket, CPU/ICU modules and marker to the same isolated container',async()=>{
    const f=fixture(),paths=[];let socket,container;
    f.s.syncManagedFexModules=p=>paths.push(p+'/fex');
    f.s.syncManagedIcuModules=p=>paths.push(p+'/icu');
    f.s.writeVersionMarker=p=>{paths.push(p);return true;};
    f.s.ensureRuntimeInstalled=async()=>true;
    f.napi.startServer=p=>{socket=p;return true;};
    f.napi.launchClient=(...args)=>{container=args[8];Object.assign(f.snapshot(),{generation:1,active:false,state:'ready'});return 0;};
    assert.equal(await f.s.startSession('desktop','desktop','/games','clean'),true);
    const root='/data/storage/el2/base/files/containers/clean/prefix';
    assert.equal(container,'clean');assert.equal(socket,root+'/wine-wayland');
    assert.deepEqual(paths,[root+'/fex',root+'/icu',root+'/.winehua-prefix-runtime-version']);
    assert.equal(paths.some(p=>p.includes('/files/.wine/')),false);
  });
  await test('clean prerequisite cannot enqueue a desktop window for the immediately destroyed root',async()=>{
    const f=fixture();let guard;
    f.s.ensureReady=async(mode,presentation,home,current)=>{guard=current;return false;};
    assert.equal(await f.s.ensureSmokeReady('desktop','desktop','/games','clean'),false);
    assert.equal(typeof guard,'function');assert.equal(guard(),false);
    assert.equal(await f.s.ensureSmokeReady('desktop','desktop','/games','reuse'),false);
    assert.equal(guard,undefined,'normal product readiness keeps its foreground behavior');
  });
  console.log(`${count} production Wine startup cases passed`);
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>clearTimeout(watchdog));
