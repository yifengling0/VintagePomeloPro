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
  let write=()=>Promise.resolve(), subscriptionFailure=false;
  const pushed=[], written=[];
  const board={on(_,fn){if(subscriptionFailure)throw Error('denied');listener=fn;}, off(){listener=undefined;},
    getData(){return read();}, setData(value){written.push(value.getPrimaryText());return write();}};
  const context={exports:{}, Promise,
    setInterval(fn){tick=fn;return 1;},clearInterval(){tick=undefined;},
    require(name){if(name==='@kit.BasicServicesKit')return {pasteboard:{getSystemPasteboard:()=>board,createData:(_,text)=>data(text),MIMETYPE_TEXT_PLAIN:'text/plain'}};
      if(name==='libdisplay_route_napi.so')return {default:{getDisplayRouteClipboard:()=>({...guest}),setDisplayRouteClipboard:text=>pushed.push(text)}};
      throw Error(name);}};
  vm.runInNewContext(compiled,context);
  return {service:context.exports.X11ClipboardService,pushed,written,
    read(fn){read=fn;},write(fn){write=fn;},deny(){subscriptionFailure=true;},
    guest(text){guest={version:guest.version+1,text};},changed(){listener?.();},tick(){tick?.();},hasTimer:()=>!!tick};
}
(async()=>{
  let f=fixture();f.service.start();await flush();assert.deepEqual(f.pushed,['system']);
  f.guest('中文');f.tick();await flush();assert.deepEqual(f.written,['中文']);
  f.read(()=>Promise.resolve(data('中文')));f.changed();await flush();assert.equal(f.pushed.length,1);
  f.service.stop();assert.equal(f.hasTimer(),false);

  f=fixture();const old=deferred();f.read(()=>old.promise);f.service.start();f.service.stop();
  f.read(()=>Promise.resolve(data('new session')));f.service.start();await flush();
  old.resolve(data('old session'));await flush();assert.deepEqual(f.pushed,['new session']);f.service.stop();

  f=fixture();f.service.start();await flush();const first=deferred(),second=deferred();let n=0;
  f.read(()=> (++n===1?first:second).promise);f.changed();f.changed();second.resolve(data('latest'));await flush();
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
  console.log('PASS production X11 clipboard: bidirectional text, epochs, stale reads, serial writes, denied permissions and retry');
})().catch(error=>{console.error(error);process.exitCode=1;});
