// Run the shipped browser policy and production stopped-client preparation.
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const assert = require('node:assert/strict'), crypto = require('node:crypto');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
const asset = path.join(root, 'entry/src/main/resources/rawfile/windows-steam');
const script = fs.readFileSync(path.join(asset, 'library-network.js'), 'utf8');
const identity = JSON.parse(fs.readFileSync(path.join(asset, 'library-network.json'), 'utf8'));
const original = fs.readFileSync(path.join(root, 'host_tests/fixtures/steam-library-bootstrap.html'), 'utf8');
const previous = fs.readFileSync(path.join(root, 'host_tests/fixtures/steam-library-previous-policy.html'), 'utf8');
const hash = text => crypto.createHash('sha256').update(text).digest('hex');
const patched = original.replace('<script defer="defer"', '<script>' + script + '</script><script defer="defer"');
assert.equal(hash(original), identity.originalSha256);
assert.equal(hash(previous), identity.previousPatchedSha256);
assert.equal(hash(script), identity.scriptSha256);
assert.equal(hash(patched), identity.patchedSha256);

let now = 0, timerId = 0;
const timers = new Map();
function advance(ms) {
  now += ms;
  for (const [id, timer] of [...timers]) if (timer.at <= now) { timers.delete(id); timer.fn(); }
}
class XHR {
  constructor() {
    this.readyState = 0; this._timeout = 0; this._timer = null; this._async = true;
    this.events = []; this.listeners = new Map(); this.aborts = 0; this.timeouts = 0;
  }
  get timeout() { return this._timeout; }
  set timeout(value) {
    if (!this._async && value !== 0) throw Error('InvalidAccessError');
    this._timeout = value;
    if (this.sent && this.readyState === 1) this.schedule();
  }
  clear() { timers.delete(this._timer); this._timer = null; }
  schedule() {
    this.clear();
    if (this._timeout) {
      this._timer = ++timerId;
      timers.set(this._timer, { at: this.sentAt + this._timeout, fn: () => {
        this._timer = null; this.readyState = 4; this.sent = false; this.timeouts++;
        this.event('timeout'); this.event('loadend');
      } });
    }
  }
  addEventListener(name, fn) { if (!this.listeners.has(name)) this.listeners.set(name, new Set()); this.listeners.get(name).add(fn); }
  removeEventListener(name, fn) { this.listeners.get(name)?.delete(fn); }
  event(name) { this.events.push(name); for (const fn of [...(this.listeners.get(name) || [])]) fn(); }
  open(method, url, async = true) {
    if (url === 'bad-open') throw Error('bad-open');
    if (!async && this._timeout !== 0) throw Error('InvalidAccessError');
    this.clear(); this._async = !!async; this.readyState = 1; this.sent = false;
  }
  send(body) {
    if (this.readyState !== 1 || this.sent || body === 'bad-send') throw Error('bad-send');
    this.sent = true; this.sentAt = now; this.schedule();
    if (body === 'immediate') this.complete();
  }
  abort() { this.clear(); this.sent = false; this.aborts++; this.readyState = 0; this.event('abort'); this.event('loadend'); }
  complete() { this.clear(); this.sent = false; this.readyState = 4; this.event('load'); this.event('loadend'); }
}
const context = { XMLHttpRequest: XHR, WeakMap, URL, window: { location: { href: 'https://steamloopback.host/index.html' } },
  setTimeout(fn, ms) { const id = ++timerId; timers.set(id, { fn, at: now + ms }); return id; },
  clearTimeout(id) { timers.delete(id); } };
vm.createContext(context);
vm.runInContext(script, context);
const open = XHR.prototype.open, send = XHR.prototype.send;
vm.runInContext(script, context);
assert.equal(XHR.prototype.open, open, 'injection is idempotent');
assert.equal(XHR.prototype.send, send);
const badge = 'https://steamcommunity.com/profiles/123/ajaxgetbadgeinfo/456/';
const optional = [badge, badge + '?l=schinese', badge.replace('https:', 'https:').replace('.com/', '.com:443/'),
  'https://store.steampowered.com/broadcast/ajaxgetstreamersforpage',
  'https://store.steampowered.com/events/ajaxgetpartnerevent'];
for (const url of optional) {
  const xhr = new XHR(); xhr.open('GET', url); xhr.timeout = 60000; xhr.send();
  assert.equal(xhr.timeout, 5000, 'native network timeout is bounded before send');
  advance(4999); assert.equal(xhr.timeouts, 0);
  advance(1); assert.equal(xhr.timeouts, 1); assert.deepEqual(xhr.events, ['timeout', 'loadend']);
  assert.equal(xhr.timeout, 60000, 'caller timeout is restored after completion');
}
for (const [method, url, async] of [
  ['POST', badge, true], ['GET', badge, false], ['GET', badge, 0], ['GET', badge, null],
  ['GET', badge.replace('https:', 'http:'), true], ['GET', badge.replace('.com/', '.com:444/'), true],
  ['GET', badge.replace('.com/', '.com.evil/'), true], ['GET', badge.replace('profiles/123', 'id/custom'), true],
  ['GET', badge + 'extra', true], ['GET', badge.replace('//steamcommunity', '//name@steamcommunity'), true],
  ['GET', 'https://store.steampowered.com/api/libraryappdetails/', true],
  ['GET', 'https://store.steampowered.com/login/', true], ['GET', 'https://steamloopback.host/images/steam_spinner.png', true],
  ['GET', 'https://cdn.steamcontent.com/depot/123/chunk/456', true], ['GET', 'not a URL', true]
]) {
  const xhr = new XHR(); xhr.open(method, url, async); xhr.send(); advance(60000); assert.equal(xhr.aborts, 0, url);
}
for (const timeout of [1, 2000, 5000]) {
  const xhr = new XHR(); xhr.open('GET', badge); xhr.timeout = timeout; xhr.send();
  assert.equal(xhr.timeout, timeout, 'shorter native timeout remains responsible');
  advance(timeout); assert.equal(xhr.timeouts, 1); assert.equal(xhr.timeout, timeout);
}
const xhr = new XHR(); xhr.open('GET', badge); xhr.send(); xhr.complete(); advance(6000);
assert.equal(xhr.timeouts, 0, 'success clears the native timeout');
xhr.open('GET', badge); xhr.send(); xhr.abort(); advance(6000); assert.equal(xhr.aborts, 1, 'manual cancel is not repeated');
xhr.open('GET', badge); xhr.send(); xhr.open('GET', 'https://store.steampowered.com/login/'); xhr.send();
advance(6000); assert.equal(xhr.timeouts, 0, 'reuse cannot time out an unrelated request');
xhr.open('GET', badge); xhr.send(); assert.throws(() => xhr.send()); advance(5000);
assert.equal(xhr.timeouts, 1, 'invalid repeated send does not disable the active timeout');
xhr.open('GET', badge); xhr.send('immediate'); advance(6000); assert.equal(xhr.timeouts, 1);
xhr.open('GET', badge); assert.throws(() => xhr.send('bad-send')); assert.equal(timers.size, 0);
xhr.open('GET', badge); xhr.send(); assert.throws(() => xhr.open('GET', 'bad-open')); advance(5000);
assert.equal(xhr.timeouts, 2, 'failed open preserves the active request timeout');
xhr.open('GET', badge); xhr.send(); xhr.timeout = 5000; xhr.complete();
assert.equal(xhr.timeout, 5000, 'explicit later write equal to our limit is preserved');
xhr.timeout = 0; xhr.open('GET', badge); xhr.send(); xhr.timeout = 1500; advance(1500);
assert.equal(xhr.timeout, 1500, 'shorter later caller setting is retained');
xhr.timeout = 0; xhr.open('GET', badge); xhr.send(); xhr.open('GET', 'local', false); xhr.send('immediate');
assert.equal(xhr.timeout, 0, 'reuse for synchronous XHR sees original timeout before native open');
xhr.open('GET', badge); xhr.send(); xhr.timeout = 9000; assert.equal(xhr.timeout, 5000); advance(5000);
assert.equal(xhr.timeout, 9000, 'longer later caller setting is restored after native timeout');
assert.equal(timers.size, 0);

const exportsObject = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root, 'entry/src/main/ets/steam/SteamLibraryNetwork.ets'), 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021 }
}).outputText, { exports: exportsObject });
(async () => {
  let text = original, running = false, writes = 0, resource = script, failWrite = false, race = false, changeDuringRead = false;
  const files = { stopped: () => !running,
    read: async () => { if (changeDuringRead && text === original) { changeDuringRead = false; text += 'custom'; } return text; },
    resource: async () => { if (race) running = true; return resource; }, hash: async value => hash(value),
    replace: async value => { if (failWrite) throw Error('interrupted-before-rename'); writes++; text = value; } };
  const prepare = () => exportsObject.prepareSteamLibraryNetwork(identity, files);
  running = true; assert.equal(await prepare(), false); assert.equal(text, original);
  running = false; resource = script + 'corrupt'; assert.equal(await prepare(), false); assert.equal(writes, 0);
  resource = script; race = true; assert.equal(await prepare(), false); assert.equal(writes, 0);
  race = false; running = false; changeDuringRead = true; assert.equal(await prepare(), false); assert.equal(writes, 0);
  text = original; failWrite = true; assert.equal(await prepare(), false); assert.equal(text, original);
  failWrite = false; assert.equal(await prepare(), true); assert.equal(text, patched); assert.equal(writes, 1);
  assert.equal(await prepare(), false); assert.equal(writes, 1, 'relaunch never duplicates policy');
  text = original + 'custom'; assert.equal(await prepare(), false); assert.equal(text, original + 'custom');
  text = previous; assert.equal(await prepare(), true); assert.equal(text, patched);
  console.log('Steam library network: scoped native 5s timeout, native outcomes, XHR reuse and atomic stopped-client preparation passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
