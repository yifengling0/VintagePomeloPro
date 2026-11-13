const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), writes = [];
const steam = '/z/SteamLinuxARM64/client/.steam/steam';
const global = steam + '/config/config.vdf';
const local = steam + '/userdata/123/config/localconfig.vdf';
const state = steam + '/config/hmosdeck-defaults-initialized.vdf';
let blocked = '', renameFailure = '', active = false;
const rules = {}, output = {};
function load(file, target, deps = {}) {
  vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(__dirname, file), 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText, { exports: target, require(id) { assert.ok(id in deps, id); return deps[id]; } });
}
load('../entry/src/main/ets/steam/SteamLocalRules.ets', rules);
const dependencies = {
  './LinuxGnuProtonComponent': { LinuxGnuProtonComponent: { ready: () => true } },
  './LinuxProtonRuntimeService': { LinuxProtonRuntimeService: { getInstance: () => ({ inUse: () => active }) } },
  './LinuxSteamPaths': {}, '../steam/SteamLocalRules': rules,
  '@kit.CoreFileKit': { fileIo: {
    listFileSync: root => { assert.equal(root, steam + '/userdata'); return ['123', '../Steam', 'config']; },
    accessSync: name => files.has(name),
    readTextSync: name => { if (!files.has(name)) throw Error('missing ' + name); return files.get(name); },
    renameSync: (from, to) => { if (to === renameFailure) throw Error('rename failed'); assert.ok(files.has(from)); files.set(to, files.get(from)); files.delete(from); }
  } },
  '@kit.BasicServicesKit': {}, './ConfigurationFiles': { ConfigurationFiles: { json: (name, text) => {
    if (name !== blocked) { files.set(name, text); writes.push(name); }
  } } }, './LinuxSteamReleaseRetention': {}, './LinuxSteamLibraryLayout': {},
  './ManagedSmokePayloadService': {
    ensureSmokeDirectory: () => true, readSmokeText: name => (files.get(name) || '').trim(),
    writeSmokeText: (name, text) => { if (name !== blocked) { files.set(name, text); writes.push(name); } }
  }
};
const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/service/LinuxSteamDiagnosticData.ets'), 'utf8') +
  '\nexport const testPrepareDefaults = prepareHmosDeckToolDefaults;';
vm.runInNewContext(ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, { exports: output, require(id) { assert.ok(id in dependencies, id); return dependencies[id]; } });
const config = entries => '"InstallConfigStore" { "Software" { "Valve" { "Steam" { "Unrelated" "retain" "CompatToolMapping" { ' + entries + ' } } } } }\n';
const mappings = text => rules.parseSteamVdf(text).child('installconfigstore').child('software').child('valve').child('steam').child('compattoolmapping');
const before = config('"0" { "name" "hmosdeck_gnu_proton" "priority" "100" } ' +
  '"413150" { "name" "hmosdeck_gnu_proton" "priority" "250" } ' +
  '"695630" { "name" "hmosdeck_proton" "priority" "300" } ' +
  '"99" { "name" "hmosdeck" } "98" { "name" "hmosdeck_gnu_proton_gl" } ' +
  '"730" { "name" "SteamLinuxRuntime_sniper" "priority" "101" } ' +
  '"42" { "name" "native" "priority" "250" } ' +
  '"43" { "name" "GE-Proton-custom" "config" "keep" } ' +
  '"3127680" { "name" "FEX" }');
let result = output.initializeHmosDeckToolMappings(before, '"global" "1" "gnu_default" "1"');
let tools = mappings(result.configuration);
assert.equal(tools.child('0').text('name'), 'hmosdeck_proton');
for (const id of ['413150', '695630', '98', '99']) assert.equal(tools.child(id), undefined, 'Remove old built-in forcing, including uninstalled games');
for (const [id, name] of [['730','SteamLinuxRuntime_sniper'],['42','native'],['43','GE-Proton-custom'],['3127680','FEX']]) assert.equal(tools.child(id).text('name'), name);
assert.equal(tools.child('43').text('config'), 'keep');
assert.equal(rules.parseSteamVdf(result.configuration).child('installconfigstore').child('software').child('valve').child('steam').text('unrelated'), 'retain');
assert.deepEqual(Array.from(mappings(output.normalizeHmosDeckToolMappings('')).children.keys()), ['0'], 'Fresh/new games must not get per-app Windows forcing');
const customGlobal = config('"0" { "name" "GE-Proton-custom" "priority" "250" "config" "custom" }');
assert.equal(output.normalizeHmosDeckToolMappings(customGlobal), customGlobal);
const userChoice = config('"0" { "name" "hmosdeck_gnu_proton" } "413150" { "name" "hmosdeck_proton" "priority" "250" }');
assert.equal(output.initializeHmosDeckToolMappings(userChoice, result.initialized).configuration, userChoice, 'Never repeat migration over later user choices');
assert.throws(() => output.initializeHmosDeckToolMappings('"InstallConfigStore" {', ''));
const localBefore = '"UserLocalConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" { "413150" { "name" "hmosdeck_gnu_proton" } "42" { "name" "native" } } "Apps" { "413150" { "LaunchOptions" "--keep-arguments" } } } } } }\n';
files.set(steam + '/userdata', 'directory'); files.set(global, before); files.set(local, localBefore);
files.set('/z/Steam/config/config.vdf', before);
output.testPrepareDefaults(steam);
assert.equal(files.get(global + '.before-hmosdeck-route-v2'), before);
assert.equal(files.get(local + '.before-hmosdeck-route-v2'), localBefore);
const account = rules.parseSteamVdf(files.get(local)).child('userlocalconfigstore').child('software').child('valve').child('steam');
assert.equal(account.child('compattoolmapping').child('413150'), undefined);
assert.equal(account.child('compattoolmapping').child('42').text('name'), 'native');
assert.equal(account.child('apps').child('413150').text('launchoptions'), '--keep-arguments');
assert.equal(files.get('/z/Steam/config/config.vdf'), before);
const firstWrites = writes.length;
files.set(global, userChoice); output.testPrepareDefaults(steam);
assert.equal(files.get(global), userChoice); assert.equal(writes.length, firstWrites);
// A failed backup cannot alter the config or publish readiness.
files.delete(state); files.delete(global + '.before-hmosdeck-route-v2'); files.set(global, before);
blocked = global + '.before-hmosdeck-route-v2';
assert.throws(() => output.testPrepareDefaults(steam), /preserve|missing/);
assert.equal(files.get(global), before); assert.equal(files.has(state), false);
blocked = ''; renameFailure = local;
files.set(local, localBefore);
assert.throws(() => output.testPrepareDefaults(steam), /rename/);
assert.equal(files.has(state), false, 'Partial multi-file migration must remain retryable');
renameFailure = ''; output.testPrepareDefaults(steam); assert.ok(files.has(state));
// Parse all account configs before changing the global config.
files.delete(state); files.set(global, before); files.set(local, '"Unfinished" {');
assert.throws(() => output.testPrepareDefaults(steam)); assert.equal(files.get(global), before);
files.set(local, localBefore); active = true;
assert.throws(() => output.testPrepareDefaults(steam), /Steam/);
assert.equal(files.get(global), before); assert.equal(files.has(state), false);
active = false; output.testPrepareDefaults(steam);
active = true; output.testPrepareDefaults(steam); // Already migrated: no write while running.
console.log('PASS native-first/OHOS default, one-time global/account migration, custom tools/options, no re-pin, backup/rename failure and retry');
