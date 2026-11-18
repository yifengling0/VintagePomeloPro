// Execute the production service against asynchronous pasteboard/route fakes.
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(process.argv[2]);
const compiled = ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/X11ClipboardService.ets'), 'utf8'), {
  compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}
}).outputText;
function deferred() { let resolve, reject; const promise = new Promise((a,b) => {resolve=a;reject=b;}); return {promise,resolve,reject}; }
const data = text => ({getPrimaryText: () => text});
async function flush() { for(let i=0;i<12;++i) await Promise.resolve(); }
function fixture() {
  let tick, listener, guest={version:0,text:''}, read=()=>Promise.resolve(data('system'));
  let write=()=>Promise.resolve(), subscriptionFailure=false, acknowledge=true;
  let granted=true, available=true, plain=true, requests=0, reads=0;
  let permission=()=>Promise.resolve({authResults:[-1]});
  const ability={applicationInfo:{accessTokenId:123}}, another={applicationInfo:{accessTokenId:123}};
  const pushed=[], written=[];
  const board={on(_,fn){if(subscriptionFailure)throw Error('denied');listener=fn;}, off(){listener=undefined;},
    hasData(){return Promise.resolve(available);},hasDataType(){return plain;},
    getData(){++reads;return read();}, setData(value){written.push(value.getPrimaryText());return write();}};
  const context={exports:{}, Promise, Date, setTimeout, clearTimeout,
    setInterval(fn){tick=fn;return 1;},clearInterval(){tick=undefined;},
    require(name){if(name==='@kit.BasicServicesKit')return {pasteboard:{getSystemPasteboard:()=>board,createData:(_,text)=>data(text),MIMETYPE_TEXT_PLAIN:'text/plain'}};
      if(name==='@kit.AbilityKit')return {abilityAccessCtrl:{GrantStatus:{PERMISSION_GRANTED:0},createAtManager:()=>({
        checkAccessTokenSync:()=>granted?0:-1,requestPermissionsFromUser:async(ctx,permissions)=>{
          assert.equal(ctx.applicationInfo.accessTokenId,123);assert.equal(permissions[0],'ohos.permission.READ_PASTEBOARD');
          ++requests;const result=await permission();granted=result.authResults[0]===0;return result;
        }})}};
      if(name==='libdisplay_route_napi.so')return {default:{getDisplayRouteClipboard:()=>({...guest}),setDisplayRouteClipboard:text=>{pushed.push(text);if(acknowledge)guest={version:guest.version+1,text};}}};
      throw Error(name);}};
  vm.runInNewContext(compiled,context);
  context.exports.X11ClipboardService.foreground(ability);
  return {service:context.exports.X11ClipboardService,pushed,written,
    ack(value){acknowledge=value;},read(fn){read=fn;},write(fn){write=fn;},deny(){subscriptionFailure=true;},
    guest(text){guest={version:guest.version+1,text};},changed(){listener?.();},tick(){tick?.();},hasTimer:()=>!!tick,
    grant(value){granted=value;},permission(fn){permission=fn;},metadata(hasData,isText=true){available=hasData;plain=isText;},
    requests:()=>requests,reads:()=>reads,background(){context.exports.X11ClipboardService.background(ability);},
    foreground(){context.exports.X11ClipboardService.foreground(ability);},
    switchWindow(){context.exports.X11ClipboardService.foreground(another);context.exports.X11ClipboardService.background(ability);}};
}
(async()=>{
  {
    const f=fixture();
    assert.equal(await f.service.systemTextAvailable(),false,'Inactive sessions never expose a paste action');
    f.grant(false);f.metadata(false,true);f.service.start();await flush();
    assert.equal(await f.service.systemTextAvailable(),false);
    f.metadata(true,false);assert.equal(await f.service.systemTextAvailable(),false,'Image-only clipboard is not plain text');
    f.metadata(true,true);assert.equal(await f.service.systemTextAvailable(),true);
    assert.equal(f.reads(),0,'Opening the menu checks metadata without reading clipboard contents');
    assert.equal(f.requests(),0,'Opening the menu does not trigger clipboard permission');
    f.background();assert.equal(await f.service.systemTextAvailable(),false);
    f.service.stop();
  }
  let f=fixture();f.service.start();await flush();assert.deepEqual(f.pushed,['system']);
  f.guest('中文');f.tick();await flush();assert.deepEqual(f.written,['中文']);
  f.read(()=>Promise.resolve(data('中文')));f.changed();await flush();assert.equal(f.pushed.length,1);
  f.service.stop();assert.equal(f.hasTimer(),false);

  f=fixture();const old=deferred();f.read(()=>old.promise);f.service.start();f.service.stop();
  f.read(()=>Promise.resolve(data('new session')));f.service.start();await flush();
  old.resolve(data('old session'));await flush();assert.deepEqual(f.pushed,['new session']);f.service.stop();

  f=fixture();f.service.start();await flush();const first=deferred(),second=deferred();let n=0;
  f.read(()=> (++n===1?first:second).promise);f.changed();await flush();f.changed();await flush();second.resolve(data('latest'));await flush();
  first.resolve(data('stale'));await flush();assert.equal(f.pushed.at(-1),'latest');assert(!f.pushed.includes('stale'));
  f.service.stop();

  f=fixture();f.service.start();await flush();const slow=deferred();f.write(()=>slow.promise);
  f.guest('old guest');f.tick();f.service.stop();f.service.start();f.guest('new guest');f.tick();
  assert.deepEqual(f.written,['old guest']); // no concurrent platform writes across epochs
  slow.resolve();await flush();f.write(()=>Promise.resolve());f.tick();await flush();
  assert.deepEqual(f.written,['old guest','new guest']);f.service.stop();

  f=fixture();f.deny();f.read(()=>Promise.reject(Error('permission')));f.service.start();await flush();
  assert(f.hasTimer());f.guest('retry');f.write(()=>Promise.reject(Error('busy')));f.tick();await flush();
  f.write(()=>Promise.resolve());f.tick();await flush();assert.deepEqual(f.written,['retry','retry']);
  f.guest('x'.repeat(262145));f.tick();await flush();assert.equal(f.written.length,2);f.service.stop();
  f=fixture();f.service.start();await flush();f.read(()=>Promise.resolve(data('authorized system')));
  assert.equal(await f.service.prepareSystemPaste(),true);assert.equal(f.pushed.at(-1),'authorized system');
  f.tick();await flush();assert.equal(f.written.length,0);f.service.stop();
  f=fixture();f.service.start();await flush();f.ack(false);f.read(()=>Promise.resolve(data('unapplied')));
  assert.equal(await f.service.prepareSystemPaste(),false);f.service.stop();
  f=fixture();f.service.start();await flush();f.read(()=>Promise.reject(Error('permission denied')));
  assert.equal(await f.service.prepareSystemPaste(),false);f.service.stop();
  f=fixture();f.service.start();await flush();const paste=deferred();f.read(()=>paste.promise);
  const pendingPaste=f.service.prepareSystemPaste();f.service.stop();paste.resolve(data('obsolete'));
  assert.equal(await pendingPaste,false);assert(!f.pushed.includes('obsolete'));
  f=fixture();f.service.start();await flush();f.read(()=>new Promise(()=>{}));
  assert.equal(await f.service.prepareSystemPaste(),false);f.service.stop();

  f=fixture();f.grant(false);f.permission(()=>Promise.resolve({authResults:[0]}));f.service.start();await flush();
  assert.equal(f.requests(),1);assert.deepEqual(f.pushed,['system']);f.changed();await flush();assert.equal(f.requests(),1);f.service.stop();
  f=fixture();f.grant(false);f.service.start();await flush();
  assert.equal(f.requests(),1);assert.equal(f.reads(),0);f.changed();await flush();assert.equal(f.requests(),1);
  f.guest('Windows copy after denial');f.tick();await flush();assert.deepEqual(f.written,['Windows copy after denial']);
  f.read(()=>Promise.resolve(data('PasteButton temporary grant')));assert.equal(await f.service.prepareSystemPaste(),true);f.service.stop();
  for(const metadata of [[false,true],[true,false]]){
    f=fixture();f.grant(false);f.metadata(...metadata);f.service.start();await flush();
    assert.equal(f.requests(),0);assert.equal(f.reads(),0);f.service.stop();
  }
  f=fixture();f.service.start();await flush();f.background();const readCount=f.reads();
  f.changed();f.tick();await flush();assert.equal(f.reads(),readCount);
  f.read(()=>Promise.resolve(data('foreground text')));f.foreground();await flush();assert.equal(f.pushed.at(-1),'foreground text');
  f.switchWindow();f.read(()=>Promise.resolve(data('another game window')));f.changed();await flush();
  assert.equal(f.pushed.at(-1),'another game window');f.service.stop();

  f=fixture();f.grant(false);const dialog=deferred();f.permission(()=>dialog.promise);f.service.start();await flush();
  f.changed();await flush();dialog.resolve({authResults:[0]});await flush();await flush();
  assert.equal(f.requests(),1);assert.equal(f.pushed.at(-1),'system');f.service.stop();
  f=fixture();f.grant(false);const oldDialog=deferred();f.permission(()=>oldDialog.promise);f.service.start();await flush();
  f.service.stop();oldDialog.resolve({authResults:[0]});await flush();assert.equal(f.pushed.length,0);
  f=fixture();f.grant(false);const waitingDialog=deferred();f.permission(()=>waitingDialog.promise);f.service.start();await flush();
  f.guest('copy while permission dialog is open');f.tick();await flush();assert.equal(f.written.length,0);
  waitingDialog.resolve({authResults:[-1]});await flush();f.tick();await flush();assert.equal(f.written.length,1);f.service.stop();

  f=fixture();f.service.start();await flush();const pending=Array.from({length:4},deferred);let count=0;
  f.read(()=>pending[count++].promise);
  for(let i=0;i<5;++i){f.changed();await flush();}
  assert.equal(count,4);f.read(()=>Promise.resolve(data('latest after bounded reads')));
  pending[0].resolve(data('obsolete bounded read'));await flush();await flush();
  assert.equal(f.pushed.at(-1),'latest after bounded reads');
  pending.slice(1).forEach(p=>p.resolve(data('obsolete bounded read')));await flush();f.service.stop();
  console.log('PASS production clipboard: permission grant/denial, PasteButton fallback, foreground ownership, text metadata, bounded reads, epochs, bidirectional text and serial writes');
})().catch(error=>{console.error(error);process.exitCode=1;});
