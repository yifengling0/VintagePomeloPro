const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'entry/src/main/ets/service/SteamStartupDiagnostics.ets'), 'utf8');
const files = new Map(), links = new Set(), descriptors = new Map(), reads = [], opened = [];
let serial = 0, shortReads = false;
function stat(name) {
  const directory = [...files.keys()].some(p => p.startsWith(name + '/'));
  if (!files.has(name) && !directory) throw Error('ENOENT');
  return {size: files.get(name)?.length || 0, mtime: 1234, isFile: () => files.has(name),
    isDirectory: () => directory && !files.has(name), isSymbolicLink: () => links.has(name)};
}
const fileIo = {
  OpenMode: {READ_ONLY: 1, NOFOLLOW: 2}, lstatSync: stat,
  statSync: key => stat(typeof key === 'number' ? descriptors.get(key) : key),
  openSync(name) {assert.equal(links.has(name), false); stat(name); opened.push(name); const fd = ++serial; descriptors.set(fd, name); return {fd};},
  closeSync(file) {assert.ok(descriptors.delete(file.fd));},
  readTextSync(name) {stat(name); return files.get(name).toString('utf8');},
  listFileSync(name) {return [...new Set([...files.keys()].filter(p => p.startsWith(name + '/')).map(p => p.slice(name.length + 1).split('/')[0]))];},
  readSync(fd, bytes, {offset, length}) {
    const name = descriptors.get(fd); assert.ok(name); reads.push({name, offset, length});
    return files.get(name).copy(Buffer.from(bytes), 0, offset, offset + Math.min(length, shortReads ? 53 : length));
  }
};
const rules = {};
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
  'entry/src/main/ets/steam/SteamLocalRules.ets'), 'utf8'), {compilerOptions: {
  target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
}}).outputText, {exports: rules});
const exportsObject = {};
vm.runInNewContext(ts.transpileModule(source, {compilerOptions: {
  target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
}}).outputText, {exports: exportsObject, Uint8Array, ArrayBuffer, setTimeout, require(id) {
  if (id === '../steam/SteamLocalRules') return rules;
  if (id === '@kit.CoreFileKit') return {fileIo};
  if (id === '@ohos.util') return {default: {TextDecoder: {create: () => ({decodeWithStream: b => new TextDecoder().decode(b)})},
    TextEncoder: class {encodeInto(s) {return new TextEncoder().encode(s);}}}};
  throw Error(id);
}});
async function runTests() {
const {collectSteamStartupDiagnostics: collect, redactSteamDiagnostic: redact} = exportsObject;
const base = '/data/storage/el2/base/files/steamohos-native';
const set = (name, value) => files.set(name, Buffer.from(value));
assert.equal(await collect(), '', 'no diagnostics should produce no report');
set(base + '/client-native.private.log', 'retired-linux-private-log');
assert.equal(await collect(), '', 'retired Linux locations are not read');
assert.equal(redact('password: secret\nrefresh-token=abc\nOK\n'), 'password: <redacted>\nrefresh-token=<redacted>\nOK\n');
let result;
const wineTemp = '/data/storage/el2/base/files/.wine/drive_c/windows/temp';
const downloads = '/storage/test';
const win = downloads + '/Steam';
const legacy = downloads + '/games/Steam-old/Steam';
const cSteam = '/data/storage/el2/base/files/.wine/drive_c/Program Files (x86)/Steam';
for (const directory of [win, legacy, cSteam]) {
  set(directory + '/steam.exe', 'MZ');
  set(directory + '/logs/cloud_log.txt', `Cloud upload failed from ${directory}\n`);
  set(directory + '/logs/content_log.txt', 'Content update failed\nhttps://cdn.test/depot?t=secret-cdn&k=secret-key\n');
  set(directory + '/config/loginusers.vdf', 'account-secret');
  set(directory + '/userdata/123/remote/save.dat', 'save-secret');
}
const persistent = '/data/storage/el2/base/files/wine-logs/wine_stderr_20261023.log';
set(persistent, '=== PID=341 role=wine ===\nerr:d3d failed texture format 0x31545844\n');
set(wineTemp + '/GuJian_d3d9.log', 'DXVK adapter and unsupported format\n');
set(wineTemp + '/unrelated.log', 'not-a-diagnostic');
set('/data/storage/el2/base/files/containers/game1/prefix/drive_c/windows/temp/Test_dxgi.log', 'Container DXGI failure\n');
result = await collect(downloads);
for (const needle of ['Windows Steam ' + win, 'Windows Steam ' + legacy, 'Windows Steam ' + cSteam,
  'Cloud upload failed', 'err:d3d failed texture format', 'DXVK adapter', 'Container DXGI failure']) assert.ok(result.includes(needle), needle);
for (const secret of ['secret-cdn', 'secret-key', 'save-secret', 'account-secret', 'not-a-diagnostic']) assert.ok(!result.includes(secret), secret);
links.add(legacy); links.add(persistent); links.add(wineTemp + '/GuJian_d3d9.log');
result = await collect(downloads);
assert.ok(!result.includes('Cloud upload failed from ' + legacy));
assert.ok(!result.includes('err:d3d failed texture format'));
assert.ok(!result.includes('DXVK adapter'));
links.clear();
set(downloads + '/games/no-steam/logs/content_log.txt', 'not-a-client');
assert.ok(!(await collect(downloads)).includes('not-a-client'));
assert.ok(!opened.some(p => /loginusers|Cookies|steamapps|userdata/.test(p)));
assert.equal(descriptors.size, 0);
assert.ok(!redact('"password" "vdf-secret"\nhttps://cdn.test/file?token-secret&k=another-secret').includes('secret'));
console.log('Windows Steam, legacy Windows installs, previous-day persistent Wine stderr, container DXVK, CDN credentials and unsafe/missing roots: PASS');

shortReads = true;
const fullResult = await collect(downloads);
shortReads = false;
assert.equal((await collect(downloads)).replace(/Collected=.*\n/, ''), fullResult.replace(/Collected=.*\n/, ''), 'partial reads preserve the full tail');
assert.ok(!opened.some(p => p.includes('steamohos-native')), 'no retired Linux diagnostics I/O');
// Flood all eligible logs: output and individual reads remain bounded, while
// each source explicitly records tail offsets/omitted files.
for (const [name] of files) {
  if (name.endsWith('.log') || name.endsWith('_log.txt')) set(name, 'a'.repeat(1023) + '\n' + ('trace line\n'.repeat(300000)));
}
let heartbeats = 0;
const heartbeat = setInterval(() => {++heartbeats;}, 1);
try { result = await collect(downloads); } finally { clearInterval(heartbeat); }
assert.ok(heartbeats >= 20, 'collection yields to the UI/event loop between bounded sources');
assert.ok(Buffer.byteLength(result) < 9 * 1024 * 1024, 'global report byte cap includes Unicode and metadata');
assert.match(result, /truncated=true/);
assert.ok(reads.every(r => r.length <= 65536));
assert.equal(descriptors.size, 0);
console.log('Diagnostic flood stays within aggregate byte budget with explicit tail markers: PASS');

async function exportLifecycleTests() {
  files.clear(); opened.length = 0; reads.length = 0;
  const logDir = downloads + '/logs', cache = '/data/storage/el2/base/cache';
  const snapshots = cache + '/log-export';
  for (let i = 1; i <= 20; ++i) set(`${logDir}/app-202610${String(i).padStart(2, '0')}.log`,
    'password=private-password\n' + 'normal log\n'.repeat(10000) + 'https://cdn.test/file?t=private-url\n');
  set(`${logDir}/native-20261024.log`, 'trace\n'.repeat(400000) + 'last error\nAuthorization: Bearer private-authorization\n');
  set(`${logDir}/native-20261023.log`, 'linked-secret'); links.add(`${logDir}/native-20261023.log`);
  set(`${logDir}/user-config.log`, 'private-config');
  set(base + '/session/state.json', '{"state":"running"}');
  set(persistent, 'err:game failed\n');
  const original = new Map(files);
  fileIo.unlinkSync = p => {if (!files.delete(p)) throw Error('ENOENT');};
  const config = {write: (p, b) => files.set(p, Buffer.from(b)), json: (p, s) => set(p, s)};
  const output = {};
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
    'entry/src/main/ets/service/LogExport.ets'), 'utf8'), {compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  }}).outputText, {exports: output, Uint8Array, ArrayBuffer, setTimeout, require(id) {
    if (id === './SteamStartupDiagnostics') return exportsObject;
    if (id === './ConfigurationFiles') return {ConfigurationFiles: config};
    if (id === '@kit.CoreFileKit') return {fileIo};
    if (id === '@ohos.util') return {default: {TextEncoder: class {encodeInto(s) {return new TextEncoder().encode(s);}}}};
    throw Error(id);
  }});
  let count = 0, fail = false, backgroundActive = false, backgroundJobs = 0;
  const originalRead = fileIo.readSync;
  fileIo.readSync = (...args) => {
    assert.ok(backgroundActive, 'all source log reads run behind the taskpool boundary');
    return originalRead(...args);
  };
  const worker = {};
  const workerSource = fs.readFileSync(path.join(root, 'entry/src/main/ets/service/LogExportTask.ets'), 'utf8');
  assert.match(workerSource, /@Concurrent\s+export async function exportLogZipInBackground/);
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
    'entry/src/main/ets/service/LogExportTask.ets'), 'utf8'), {compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  }}).outputText, {exports: worker, require(id) {
    if (id === './LogExport') return output;
    if (id === '@kit.CoreFileKit') return {fileIo};
    if (id === '@kit.PerformanceAnalysisKit') return {hilog: {info() {}}};
    if (id === '@kit.BasicServicesKit') return {zlib: {async compressFiles(paths, destination) {
      assert.ok(backgroundActive, 'ZIP compression also stays behind the taskpool boundary');
      ++count; assert.ok(paths.some(p => p.endsWith('/steam-startup-diagnostics.log')));
      assert.ok(paths.some(p => p.endsWith('/export-manifest.log')));
      let size = 0;
      for (const p of paths) {
        assert.ok(p.startsWith(snapshots + '/'), 'ZIP contains snapshots, never live raw files');
        const body = files.get(p).toString('utf8'); size += Buffer.byteLength(body);
        for (const secret of ['private-password', 'private-url', 'private-authorization', 'linked-secret', 'private-config']) assert.ok(!body.includes(secret), secret);
      }
      assert.ok(size < 25 * 1024 * 1024);
      assert.ok(files.get(snapshots + '/export-manifest.log').includes('omitted'));
      set(destination, 'partial ZIP');
      await new Promise(resolve => setTimeout(resolve, 5));
      if (fail) throw Error('compression failed');
    }}};
    throw Error(id);
  }});
  const logService = {};
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
    'entry/src/main/ets/service/LogService.ets'), 'utf8'), {compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  }}).outputText, {exports: logService, require(id) {
    if (id === './LogExportTask') return worker;
    if (id === '@kit.CoreFileKit') return {fileIo, fileUri: {getUriFromPath: p => 'file://' + p}};
    if (id === '@kit.AbilityKit') return {bundleManager: {getBundleInfoForSelfSync: () => ({name: 'com.vintage.pomelopro', versionName: '1.5.1', versionCode: 1005010})}};
    if (id === '@kit.PerformanceAnalysisKit') return {hilog: {info() {}}};
    if (id === 'libentry.so') return {default: {}};
    if (id === '@kit.ArkTS') return {taskpool: {async execute(fn, ...args) {
      assert.equal(fn, worker.exportLogZipInBackground);
      assert.ok(args.every(a => typeof a === 'string'), 'only small serializable values cross threads');
      ++backgroundJobs; backgroundActive = true;
      try { return await fn(...args); } finally { backgroundActive = false; }
    }}};
    throw Error(id);
  }});
  const {AppLog} = logService;
  AppLog.inited = true; AppLog.logDir = logDir;
  AppLog.ensureFd = () => {}; AppLog.ensureDir = () => true;
  assert.equal(AppLog.formatLine('tag', '%{private}s %{public}d %s', ['private-value', 42, 'hello']), '<private> 42 hello');
  const [a, b] = await Promise.all([AppLog.exportLogsZip(), AppLog.exportLogsZip()]);
  assert.equal(a, b); assert.equal(count, 1, 'concurrent exports share one snapshot/compression');
  assert.equal(backgroundJobs, 1);
  assert.ok(![...files.keys()].some(p => p.startsWith(snapshots + '/')), 'snapshot cleanup after success');
  for (const [p, value] of original) assert.equal(files.get(p), value, 'source files unchanged');
  files.delete(a.slice('file://'.length));
  fail = true;
  await assert.rejects(AppLog.exportLogsZip(), /compression failed/);
  assert.ok(![...files.keys()].some(p => p.startsWith(snapshots + '/') || /\/logs-.*\.zip$/.test(p)), 'no partial ZIP or snapshots after failure');
  assert.equal(descriptors.size, 0);
  fileIo.readSync = originalRead;
  links.clear();
  console.log('Production export lifecycle: sanitized snapshots, global quotas, concurrent requests, source preservation, cleanup on success/failure and private formatter: PASS');
}
function initializationRecoveryTests() {
  const loaded = {}, dirs = new Set(), handles = new Set(), writes = [];
  let ready = false, clock = 10000, opens = 0, attempts = 0, nativeCalls = 0;
  const io = {OpenMode: {READ_WRITE: 1, CREATE: 2, APPEND: 4},
    statSync(p) {
      if (typeof p === 'number' && handles.has(p)) return {size: 0};
      if (dirs.has(p)) return {isDirectory: () => true};
      throw Error('ENOENT');
    },
    mkdirSync(p) {++attempts; if (!ready) throw Error('permission pending'); dirs.add(p);},
    openSync() {const fd = ++opens; handles.add(fd); return {fd};},
    closeSync(fd) {assert.ok(handles.delete(fd));},
    writeSync(fd, text) {assert.ok(handles.has(fd)); writes.push(text);},
    listFileSync() {return [];}
  };
  class Clock extends Date {static now() {return clock;}}
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
    'entry/src/main/ets/service/LogService.ets'), 'utf8'), {compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  }}).outputText, {exports: loaded, Date: Clock, require(id) {
    if (id === './LogExportTask') return {};
    if (id === '@kit.CoreFileKit') return {fileIo: io, fileUri: {
      FileUri: class {constructor(uri) {this.path = uri.replace('file://docs', '');}}
    }};
    if (id === '@kit.AbilityKit') return {bundleManager: {BundleFlag: {GET_BUNDLE_INFO_WITH_APPLICATION: 1},
      getBundleInfoForSelfSync: () => ({name: 'com.vintage.pomelopro'})}};
    if (id === '@kit.PerformanceAnalysisKit') return {hilog: {info() {}, error() {}}};
    if (id === 'libentry.so') return {default: {initAppLog() {++nativeCalls;}}};
    if (id === '@kit.ArkTS') return {};
    throw Error(id);
  }});
  const {AppLog} = loaded;
  AppLog.init();
  assert.equal(nativeCalls, 0);
  for (let i = 0; i < 20; ++i) AppLog.write('I', 'test', 'pending', []);
  assert.equal(attempts, 1, 'permission failures are throttled during startup');
  ready = true; clock += 5000;
  AppLog.write('I', 'test', 'permission ready', []);
  assert.equal(nativeCalls, 1, 'later writes recover the native sink without restarting the app');
  assert.ok(writes.some(s => s.includes('permission ready')));
  AppLog.init();
  assert.equal(nativeCalls, 1, 'successful initialization remains idempotent');
  AppLog.inited = false; AppLog.nextInitRetryAt = clock + 5000;
  AppLog.init(true);
  assert.equal(nativeCalls, 2, 'explicit export can retry immediately');
  console.log('First-install logging: delayed permission recovery, bounded retries and immediate export retry PASS');
}
initializationRecoveryTests();
await exportLifecycleTests();
}
runTests().catch(error => {console.error(error); process.exitCode = 1;});
