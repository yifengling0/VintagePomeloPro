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
const {collectSteamStartupDiagnostics: collect, redactSteamDiagnostic: redact, summarizeSteamToolSelection: summarize} = exportsObject;
const base = '/data/storage/el2/base/files/steamohos-native';
const steam = '/storage/test/SteamLinuxARM64';
const set = (name, value) => files.set(name, Buffer.from(value));
assert.equal(await collect(), '', 'no Steam files cannot add a misleading historic report');
set(base + '/session/state.json', '{"state":"starting"}');
set(base + '/session/native-startup.json', '{"stage":"steam_client_starting"}');
set(base + '/external-root.path', steam);
set(steam + '/client/.steam/steam/logs/cef_log.txt', 'GL init failed\nAuthorization: Bearer secret123\naccess_token="secret456"\naccountname=testuser\n76561198012345678 user@example.test\nRenderer terminated\n');
set(base + '/client-native.private.log', 'password=' + 's'.repeat(70000) + '\nNative loader failure errno=13\n');
let result = await collect();
assert.match(result, /steam_client_starting/);
assert.match(result, /Native loader failure errno=13/);
assert.match(result, /GL init failed/);
for (const secret of ['secret123','secret456','testuser','76561198012345678','user@example.test','s'.repeat(80)]) assert.ok(!result.includes(secret), secret);
assert.ok(reads.every(r => r.length <= 65536), 'per-file reads are bounded');
assert.ok(result.length < 600000);
assert.equal(descriptors.size, 0, 'all open handles are closed');
assert.ok(!opened.some(p => /loginusers|Cookies|steamapps|userdata/.test(p)), 'fixed log allowlist cannot read credentials or games');
shortReads = true;
assert.equal((await collect()).replace(/Collected=.*\n/, ''), result.replace(/Collected=.*\n/, ''), 'short successful reads must not truncate the tail');
shortReads = false;
links.add(base + '/client-native.private.log');
result = await collect();
assert.ok(!result.includes('Native loader failure'));
links.clear();
set(base + '/external-root.path', '/storage/test/../other/SteamLinuxARM64');
reads.length = 0; await collect();
assert.ok(!reads.some(r => r.name.includes('cef_log.txt')), 'invalid root is never followed');
set(base + '/external-root.path', 'x'.repeat(5000));
assert.match(await collect(), /Steam log root unavailable/);
assert.equal(redact('password: "x"\nrefresh-token=abc\nOK\n'), 'password: <redacted>\nrefresh-token=<redacted>\nOK\n');
assert.equal(redact('-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----'), '<private key>');
console.log('Steam diagnostic export: bounded and partial reads, tail boundary, credential redaction, missing files, fixed allowlist, path validation and closed handles PASS');

const date = new Date();
const day = date.getFullYear().toString() + (date.getMonth() + 1).toString().padStart(2, '0') +
  date.getDate().toString().padStart(2, '0');
const stderr = '/data/storage/el2/base/temp/wine_stderr_' + day + '.log';
set(stderr, 'older lines\n'.repeat(7000) + '=== PID=340 ===\nUnhandled exception c0000005\npassword=never-export\n');
result = await collect();
assert.match(result, /=== Wine stderr .*wine_stderr_/);
assert.match(result, /=== PID=340 ===/);
assert.match(result, /Unhandled exception c0000005/);
assert.ok(!result.includes('never-export'));
assert.ok(reads.every(r => r.length <= 65536));
links.add(stderr);
assert.ok(!(await collect()).includes('Unhandled exception c0000005'), 'Reject a replaced stderr symlink');
links.delete(stderr); files.delete(stderr);
assert.equal(descriptors.size, 0);
console.log('Fixed daily Wine stderr tail, exception preservation, bounded reads, redaction and link rejection: PASS');

// A content/compatibility error before the broker sees a game must survive
// export even when console_log is overwhelmed by hardware-probe messages.
set(base + '/external-root.path', steam);
set(steam + '/client/.steam/steam/logs/console_log.txt', 'udev: probe\n'.repeat(12000));
set(steam + '/client/.steam/steam/logs/content_log.txt',
  'Failed updating app 227300: Missing file privileges runtime/amd64/etc\n');
set(steam + '/client/.steam/steam/logs/gameprocess_log.txt',
  'AppID 227300 starting\nHMOSDECK_STEAM_BRIDGE=1234:bridge-secret:227300\n--password \"game-secret\" --normal\n');
const launch = '/data/storage/el2/base/files/.wine/drive_c/hmosdeck/session';
set(launch + '/last-game-launch.json', '{"appId":"227300","status":"rejected","updatedAt":12345}');
set(launch + '/last-client-helper.json', '{"appId":"0","role":"driver_query"}');
const configPath = steam + '/client/.steam/steam/config/config.vdf';
set(configPath, '"InstallConfigStore" { "Software" { "Valve" { "Steam" { ' +
  '"Accounts" { "user" { "LoginKey" "never-export-this-key" } } ' +
  '"CompatToolMapping" { "0" { "name" "hmosdeck_proton" "priority" "75" } ' +
  '"227300" { "name" "proton_9" "priority" "250" } } } } } }');
result = await collect();
assert.match(result, /Missing file privileges runtime\/amd64\/etc/);
assert.match(result, /"appId":"227300","tool":"proton_9","priority":"250"/);
assert.match(result, /last game launch/);
assert.match(result, /"updatedAt":12345/);
for (const secret of ['never-export-this-key', 'bridge-secret', 'game-secret']) assert.ok(!result.includes(secret), secret);
assert.ok(result.length < 1048576, 'total diagnostic text remains bounded');
assert.ok(reads.every(r => r.length <= 65536), 'config reads are also chunked');
links.add(configPath); result = await collect();
assert.match(result, /compatibility tool selection: unavailable/);
assert.ok(!result.includes('"tool":"proton_9"'));
links.clear();
set(configPath, 'x'.repeat(1048577));
assert.match(await collect(), /compatibility tool selection: unavailable/);
assert.equal(summarize('"Unrelated" "no-account-data"'), 'No explicit compatibility-tool mappings');
assert.throws(() => summarize('"InstallConfigStore" {'));
const mappings = Array.from({length: 160}, (_, i) => '"' + (i+1) + '" { "name" "hmosdeck_proton" }').join(' ');
assert.equal(summarize('"InstallConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" {' + mappings + '} } } } }').split('\n').length, 128);
assert.equal(descriptors.size, 0);
console.log('Pre-launch file privileges, gameprocess/content logs, bounded tool selection, raw config exclusion, bridge/password redaction and link rejection: PASS');

const wineTemp = '/data/storage/el2/base/files/.wine/drive_c/windows/temp';
set(wineTemp + '/hmosdeck-steam-api-state.json', '{"stage":"interface_unavailable","interface":"SteamClient021","interfaceStatus":1}');
set(wineTemp + '/hmosdeck-steam-bridge-state.json', '{"stage":"request_completed","method":501,"status":4,"clientSourceVerified":false}');
result = await collect();
assert.match(result, /Steam API bridge client/);
assert.match(result, /"interface":"SteamClient021"/);
assert.match(result, /"clientSourceVerified":false/);
assert.equal(descriptors.size, 0);
console.log('Steam API initialization evidence exports exact interface/status independently from game and download logs: PASS');

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
  'Cloud upload failed', 'Linux ARM64 Steam', 'err:d3d failed texture format', 'DXVK adapter', 'Container DXGI failure']) assert.ok(result.includes(needle), needle);
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
console.log('Both Steam clients, legacy Windows installs, previous-day persistent Wine stderr, container DXVK, CDN credentials and unsafe/missing roots: PASS');

// A later successful game/helper must not hide the GNU failure being reported.
const gameResult = launch + '/gnu-proton-2490930.json';
const helperResult = launch + '/gnu-proton-helper-install_script-1918680.json';
set(gameResult, JSON.stringify({status: 'proton_steam_account_not_ready',
  steamReadiness: {exitCode: 5, output: 'GNU_STEAM_READY session_ready=0 local_user=0\n'},
  prefix: {output: 'x'.repeat(270000) + '\nerr:module could not load test.dll\n'}}));
set(helperResult, '{"status":"proton_game_failed","prefix":{"exitCode":53}}');
set(launch + '/game-launch-2490930.json', '{"appId":"2490930","status":"proton_steam_account_not_ready"}');
set(launch + '/last-game-launch.json', '{"appId":"1437830","status":"gnu_proton_starting"}');
set(launch + '/gnu-proton-2490930.json.secret', 'not-a-result');
set(launch + '/gnu-proton-helper-unknown-2490930.json', 'not-a-helper');
set(base + '/proton-probe/probe-runtime.log', 'GNU_STEAM_READY stage=sdk_load error=missing module\n');
set(steam + '/client/.steam/steam/logs/content_log.txt',
  'CDN: cdn.test/depot/123/manifest/456?t=bare-url-secret&k=opaque-secret\n' +
  'cdn.test:80/depot/123?auth_key=other-secret\n');
result = await collect();
for (const text of ['GNU Proton game result gnu-proton-2490930.json', 'err:module could not load test.dll',
  'GNU Proton helper result gnu-proton-helper-install_script-1918680.json', 'session_ready=0',
  'GNU_STEAM_READY stage=sdk_load', '"appId":"1437830"']) assert.ok(result.includes(text), text);
for (const secret of ['bare-url-secret', 'opaque-secret', 'other-secret', 'not-a-result', 'not-a-helper'])
  assert.ok(!result.includes(secret), secret);
links.add(gameResult);
assert.ok(!(await collect()).includes('err:module could not load test.dll'), 'reject linked GNU result');
links.clear();
links.add(launch);
assert.ok(!(await collect()).includes('err:module could not load test.dll'), 'reject linked result directory');
links.clear();
set(gameResult, 'x'.repeat(1048577));
result = await collect();
assert.match(result, /GNU Proton game result gnu-proton-2490930.json: unavailable/,
  'oversized single-line JSON is explicitly unavailable, never a misleading empty tail');
files.delete(gameResult); files.delete(helperResult);
assert.equal(descriptors.size, 0);
console.log('GNU game/helper/readiness output, large compact JSON, later launches, bare CDN queries and unsafe/oversized results: PASS');

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
await exportLifecycleTests();
}
runTests().catch(error => {console.error(error); process.exitCode = 1;});
