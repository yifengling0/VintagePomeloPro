// Test actual ArkTS rules and update transaction; no device success implied.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '../entry/src/main/ets/steam');
function load(name) {
  const exports = {};
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root, name + '.ets'), 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021 }
  }).outputText, { exports, Map, Set, Error, Promise });
  return exports;
}
const rules = load('SteamLocalRules');
const transaction = load('SteamImportTransaction');
const tree = rules.parseSteamVdf('"AppState" { "appid" "123" "name" "中文 游戏" "installdir" "有 空格" }');
assert.equal(tree.child('appstate').text('name'), '中文 游戏');
assert.equal(rules.parseSteamVdf(rules.serializeSteamVdf(tree)).child('appstate').text('installdir'), '有 空格');
for (const value of ['"a" { "b" "c"', '}', '"a"', '"a" "unterminated']) assert.throws(() => rules.parseSteamVdf(value));
assert.equal(rules.resolveSteamLibraryPath('Z:\\Steam Library', '/download', '/prefix/drive_c'), '/download/Steam Library');
assert.equal(rules.resolveSteamLibraryPath('C:\\Games', '/download', '/prefix/drive_c'), '/prefix/drive_c/Games');
assert.equal(rules.resolveSteamLibraryPath('Z:\\..\\escape', '/download', '/prefix/drive_c'), '');
assert.equal(rules.steamWindowsPath('/download/Steam/steam.exe', '/download', '/prefix/drive_c'), 'z:/Steam/steam.exe');
const argv = rules.steamLaunchArguments('play', '123', false, ['-force-d3d11', 'arg with spaces']);
assert.equal(argv.includes('-cef-disable-gpu'), false);
assert.equal(argv.includes('-cef-force-gpu'), false);
assert.deepEqual(Array.from(argv.slice(argv.indexOf('-applaunch'))), ['-applaunch','123','-force-d3d11','arg with spaces']);
assert.equal(rules.steamLaunchArguments('install', '123', false).at(-1), 'steam://install/123');
assert.equal(rules.steamLaunchArguments('open', '', true).includes('-cef-disable-gpu'), true);
assert.equal(rules.steamLaunchArguments('open', '', false).at(-1), 'steam://open/main');
for (const [action, uri] of [['downloads','steam://open/downloads'], ['store','steam://store'],
  ['account','steam://open/settings/account'], ['bigPicture','steam://open/bigpicture'], ['desktop','steam://close/bigpicture']]) {
  assert.equal(rules.steamLaunchArguments('open', '', false, [], action).at(-1), uri);
}
assert.throws(() => rules.steamLaunchArguments('open', '', false, [], 'invalid'));
assert.throws(() => rules.steamLaunchArguments('play', '123;bad', false));

async function interruption(failAt, fresh=false, prepareFailure=false) {
  const files = new Map([['stage','dir'],['stage/steam.exe','new'],['stage/config','dir'],['stage/config/login','package-login']]);
  if(!fresh) for (const [p,v] of [['live','dir'],['live/steam.exe','old'],['live/config','dir'],['live/config/login','device-login'],['live/steamapps','dir'],['live/steamapps/game','game-data']]) files.set(p,v);
  let durable={phase:'extract',hadClient:!fresh,preserved:[]},step=0,crashed=false;
  function operation() { if(crashed || ++step===failAt) {crashed=true;throw Error('power-loss');} }
  const io={
    exists:p=>files.has(p),
    async rename(a,b) { operation();assert.ok(files.has(a));assert.ok(!files.has(b));
      const subset=Array.from(files).filter(([p])=>p===a||p.startsWith(a+'/'));
      for(const [p,v] of subset){files.delete(p);files.set(b+p.slice(a.length),v);} },
    async remove(a){ operation(); for(const p of files.keys())if(p===a||p.startsWith(a+'/'))files.delete(p); },
    async journal(value){operation();durable=value?JSON.parse(JSON.stringify(value)):null;}
  };
  try {await transaction.commitSteamImport(io,'live','stage','backup',fresh?[]:['config','steamapps'], async()=>{if(prepareFailure)throw Error('prepare-failed');});}catch(e){assert.ok(['power-loss','prepare-failed'].includes(e.message));}
  crashed=false;failAt=0;
  if(durable)await transaction.recoverSteamImport(io,'live','stage','backup',durable);
  if(!fresh){assert.equal(files.get('live/config/login'),'device-login');assert.equal(files.get('live/steamapps/game'),'game-data');}
  if(files.has('live'))assert.ok(['new','old'].includes(files.get('live/steam.exe')));
  return step;
}
(async()=>{
  await interruption(0,false,true);
  await interruption(0,true,true);
  const count=await interruption(0);
  for(let i=1;i<=count;i++)await interruption(i);
  for(let i=1;i<=3;i++)await interruption(i,true);
  console.log(`Steam local: VDF, path, argv and ${count+6} transactional recovery scenarios passed`);
})().catch(e=>{console.error(e);process.exitCode=1;});
