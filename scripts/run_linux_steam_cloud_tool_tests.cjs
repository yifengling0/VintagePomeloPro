const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const files = new Map(), writes = [];
const steam = '/z/SteamLinuxARM64/client/.steam/steam';
const global = steam + '/config/config.vdf';
const local = steam + '/userdata/123/config/localconfig.vdf';
const windows = '/z/Steam/config/config.vdf';
const before = '"CompatToolMapping" { "695630" { "name"\t\t"hmosdeck" "config" "" } "42" { "name" "other_tool" } }';
files.set(global, before); files.set(local, '"name" "hmosdeck"\n"name" "hmosdeck_other"');
files.set(windows, '"name" "hmosdeck"');
let blocked = '';
const output = {};
const source = fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/LinuxSteamDiagnosticData.ets'), 'utf8') +
  '\nexport const testMigrateSelections = migrateHmosDeckToolSelections; export const testPrepareDefaults = prepareHmosDeckToolDefaults;';
const rules = {};
const rulesCode = ts.transpileModule(fs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/steam/SteamLocalRules.ets'), 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;
vm.runInNewContext(rulesCode, { exports: rules });
const dependencies = {
  './LinuxSteamPaths': {},
  '../steam/SteamLocalRules': rules,
  '@kit.CoreFileKit': { fileIo: {
    listFileSync: root => { assert.equal(root, steam + '/userdata'); return ['123', '../Steam', 'config']; },
    accessSync: name => files.has(name),
    readTextSync: name => { assert.ok(files.has(name)); return files.get(name); },
    renameSync: (from, to) => { assert.ok(files.has(from)); files.set(to, files.get(from)); files.delete(from); }
  } },
  '@kit.BasicServicesKit': {}, './ConfigurationFiles': {}, './LinuxSteamReleaseRetention': {}, './LinuxSteamLibraryLayout': {},
  './ManagedSmokePayloadService': {
    ensureSmokeDirectory: () => true,
    readSmokeText: name => files.get(name) || '',
    writeSmokeText: (name, text) => { if (name !== blocked) { files.set(name, text); writes.push(name); } }
  }
};
const code = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;
vm.runInNewContext(code, { exports: output, require(id) { assert.ok(id in dependencies, id); return dependencies[id]; } });
assert.equal(output.migrateHmosDeckToolReferences(''), '');
assert.equal(output.migrateHmosDeckToolReferences('"name" "other_tool"'), '"name" "other_tool"');
output.testMigrateSelections(steam);
assert.equal(files.get(global + '.before-hmosdeck-proton-v1'), before);
assert.equal(files.get(global), before.replace('"hmosdeck"', '"hmosdeck_proton"'));
assert.equal(files.get(local), '"name" "hmosdeck_proton"\n"name" "hmosdeck_other"');
assert.equal(files.get(windows), '"name" "hmosdeck"', 'Windows Steam must remain isolated');
const count = writes.length; output.testMigrateSelections(steam);
assert.equal(writes.length, count, 'Repeated startup must not rewrite configurations or backups');
files.delete(global + '.before-hmosdeck-proton-v1'); files.set(global, before);
blocked = global + '.before-hmosdeck-proton-v1';
assert.throws(() => output.testMigrateSelections(steam), /preserve/);
assert.equal(files.get(global), before, 'Failed backup must leave the source configuration intact');
console.log('Linux Steam Proton tool migration, original config backups, repeat startup and Windows isolation: PASS');

blocked = '';
const config = '"InstallConfigStore" { "Software" { "Valve" { "Steam" { "Unrelated" "retain-value" "CompatToolMapping" { "0" { "name" "proton_experimental" "config" "old-proton-flags" } "695630" { "name" "proton_11" } "42" { "name" "GE-Proton-custom" } "77" { "name" "proton_9" } } } } } }\n';
const normalized = output.normalizeHmosDeckToolMappings(config, ['695630', '42', '../77']);
const steamConfig = rules.parseSteamVdf(normalized).child('installconfigstore').child('software').child('valve').child('steam');
assert.equal(steamConfig.text('unrelated'), 'retain-value');
const tools = steamConfig.child('compattoolmapping');
assert.equal(tools.child('0').text('name'), 'hmosdeck_proton');
assert.equal(tools.child('0').text('config'), '');
assert.equal(tools.child('0').text('priority'), '100');
assert.equal(tools.child('695630').text('name'), 'hmosdeck_proton');
assert.equal(tools.child('695630').text('priority'), '250');
assert.equal(tools.child('42').text('name'), 'GE-Proton-custom');
assert.equal(tools.child('77').text('name'), 'proton_9', 'Do not rewrite a non-installed app');
assert.equal(output.normalizeHmosDeckToolMappings(normalized, ['695630']), normalized);
assert.ok(output.normalizeHmosDeckToolMappings('', []).includes('hmosdeck_proton'), 'Fresh clients need an OHOS tool default');
assert.throws(() => output.normalizeHmosDeckToolMappings('"InstallConfigStore" {', []));
files.set(global, config);
output.testPrepareDefaults(steam, ['695630']);
assert.equal(files.get(global + '.before-hmosdeck-default-v1'), config, 'Preserve complete original config including trailing whitespace');
assert.equal(files.get(global), normalized);
assert.equal(files.get(windows), '"name" "hmosdeck"');
const repeatWrites = writes.length;
output.testPrepareDefaults(steam, ['695630']);
assert.equal(writes.length, repeatWrites);
files.set(global, config); files.delete(global + '.before-hmosdeck-default-v1');
blocked = global + '.before-hmosdeck-default-v1';
assert.throws(() => output.testPrepareDefaults(steam, ['695630']));
assert.equal(files.get(global), config, 'Default selection must not change if its backup cannot be preserved');
blocked = '';
console.log('Linux Steam fresh default, built-in Proton migration, explicit custom-tool preservation and atomic config writes: PASS');

function mappings(text) {
  return rules.parseSteamVdf(text).child('installconfigstore').child('software').child('valve')
    .child('steam').child('compattoolmapping');
}
function configuration(entries) {
  return '"InstallConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" { ' + entries + ' } } } } }';
}
const oldDefault = configuration('"0" { "name" "hmosdeck_proton" "priority" "75" "config" "keep-flags" }');
const withCs2 = output.normalizeHmosDeckToolMappings(oldDefault, ['730', '730', '227300']);
let selections = mappings(withCs2);
assert.equal(selections.child('0').text('priority'), '100', 'Upgrade existing installations, not just fresh configs');
assert.equal(selections.child('0').text('config'), 'keep-flags');
assert.equal(selections.child('730').text('name'), 'hmosdeck_proton', 'CS2 must not inherit its native sniper mapping');
assert.equal(selections.child('730').text('priority'), '250', 'Per-game selection must beat Steam native defaults at 100/101');
assert.equal(selections.child('227300').text('priority'), '250');
assert.equal(output.normalizeHmosDeckToolMappings(withCs2, ['730', '227300']), withCs2);

const nativeMappings = configuration(
  '"730" { "name" "SteamLinuxRuntime_sniper" "priority" "100" } ' +
  '"440" { "name" "SteamLinuxRuntime_sniper" "priority" "101" } ' +
  '"570" { "name" "native" "priority" "75" } ' +
  '"42" { "name" "native" "priority" "250" "config" "explicit-native" } ' +
  '"43" { "name" "GE-Proton-custom" "priority" "250" } ' +
  '"44" { "name" "hmosdeck_proton" "priority" "75" "config" "own-flags" } ' +
  '"45" { "name" "hmosdeck_proton" "priority" "300" }');
selections = mappings(output.normalizeHmosDeckToolMappings(nativeMappings, ['730', '440', '570', '42', '43', '44', '45']));
for (const id of ['730', '440', '570', '44']) {
  assert.equal(selections.child(id).text('name'), 'hmosdeck_proton');
  assert.equal(selections.child(id).text('priority'), '250');
}
assert.equal(selections.child('42').text('name'), 'native', 'Preserve an explicit user native selection');
assert.equal(selections.child('42').text('config'), 'explicit-native');
assert.equal(selections.child('43').text('name'), 'GE-Proton-custom');
assert.equal(selections.child('44').text('config'), 'own-flags');
assert.equal(selections.child('45').text('priority'), '300', 'Do not lower an existing stronger selection');

function manifest(id, name, directory, extra = '') {
  return '"AppState" { "appid" "' + id + '" "name" "' + name + '" "installdir" "' + directory + '" ' + extra + ' }';
}
const installed = [
  ['730', manifest('730', 'Counter-Strike 2', 'Counter-Strike Global Offensive')],
  ['227300', manifest('227300', 'Euro Truck Simulator 2', 'Euro Truck Simulator 2')],
  ['695630', manifest('695630', '新仙剑奇侠传', 'NewPAL')],
  ['1628350', manifest('1628350', 'Steam Linux Runtime 3.0 (sniper)', 'SteamLinuxRuntime_sniper')],
  ['1070560', manifest('1070560', 'Steam Linux Runtime 1.0 (scout)', 'SteamLinuxRuntime')],
  ['228980', manifest('228980', 'Steamworks Common Redistributables', 'Steamworks Shared')],
  ['2805730', manifest('2805730', 'Proton 9.0', 'Proton 9.0')],
  ['3127680', manifest('3127680', 'FEX', 'FEX-Emu')],
  ['50', manifest('50', 'Other component', 'Other component', '"type" "tool"')],
  ['51', manifest('52', 'Mismatched app', 'Game')],
  ['53', manifest('53', 'Unsafe path', '../Game')],
  ['54', '"AppState" {'],
  ['55', manifest('55', '', 'Unknown')]
];
const games = installed.filter(([id, text]) => output.isHmosDeckGameManifest(text, id)).map(([id]) => id);
assert.deepEqual(games, ['730', '227300', '695630']);
selections = mappings(output.normalizeHmosDeckToolMappings('', games));
assert.deepEqual(Array.from(selections.children.keys()), ['0', '730', '227300', '695630']);
console.log('Linux Steam priority upgrade, CS2 native-default override, shared-tool exclusion, explicit selection and repeated startup: PASS');
