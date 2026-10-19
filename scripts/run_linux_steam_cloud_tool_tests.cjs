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
assert.equal(tools.child('695630').text('name'), 'hmosdeck_proton');
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
