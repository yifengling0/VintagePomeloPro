const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '..');
function load(relative, dependencies) {
  const output = {};
  const code = ts.transpileModule(fs.readFileSync(path.join(root, relative), 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText;
  vm.runInNewContext(code, { exports: output, require(id) { assert.ok(id in dependencies, id); return dependencies[id]; } });
  return output;
}
const rules = load('entry/src/main/ets/steam/SteamLocalRules.ets', {});
let failMove = 0, moved = 0, failConfig = false;
const read = name => fs.existsSync(name) ? fs.readFileSync(name, 'utf8') : '';
const helper = load('entry/src/main/ets/service/LinuxSteamLibraryLayout.ets', {
  '@kit.CoreFileKit': { fileIo: {
    listFileSync: name => fs.readdirSync(name).sort(),
    lstatSync: fs.lstatSync, accessSync: fs.existsSync, readTextSync: name => fs.readFileSync(name, "utf8"),
    renameSync: (from, to) => {
      if (!from.endsWith('.hmosdeck-write-v2') && ++moved === failMove) throw new Error('injected move');
      fs.renameSync(from, to);
    }
  } },
  '../steam/SteamLocalRules': rules,
  './ConfigurationFiles': { ConfigurationFiles: {
    fileHash: async name => crypto.createHash('sha256').update(fs.readFileSync(name)).digest('hex')
  } },
  './ManagedSmokePayloadService': {
    readSmokeText: name => read(name).trim(),
    writeSmokeText: (name, text) => {
      if (failConfig && name.includes('/library/steamapps/libraryfolders.vdf.hmosdeck-write')) throw new Error('injected config');
      fs.writeFileSync(name, text);
    },
    ensureSmokeDirectory: name => { fs.mkdirSync(name, { recursive: true }); return true; }
  }
});
function fixture(base) {
  const steam = base + '/client/.steam/steam', library = base + '/library';
  function file(relative, value) { const full = base + '/' + relative; fs.mkdirSync(path.dirname(full), { recursive: true }); fs.writeFileSync(full, value); }
  file('client/.steam/steam/steamapps/common/Steamworks Shared/redist.dat', 'public-components');
  file('client/.steam/steam/steamapps/appmanifest_228980.acf', '"AppState" { "appid" "228980" }');
  file('library/steamapps/common/NewPAL/game.exe', 'original-game');
  file('library/steamapps/appmanifest_695630.acf', '"AppState" { "appid" "695630" }');
  file('client/.steam/steam/steamapps/libraryfolders.vdf', `"libraryfolders" { "0" { "path" "${steam}" "contentid" "12345" "apps" { "228980" "132595752" } } "1" { "path" "${library}" "apps" { "695630" "1743659178" } } }`);
  file('WindowsSteam/steamapps/common/unrelated.exe', 'windows-original');
  return { steam, library, file };
}
function check(base, steam, library) {
  const folders = rules.parseSteamVdf(read(library + '/steamapps/libraryfolders.vdf')).child('libraryfolders');
  assert.equal(folders.children.size, 1);
  assert.equal(folders.child('0').text('path'), steam);
  assert.equal(folders.child('0').text('contentid'), '12345');
  assert.equal(folders.child('0').child('apps').children.size, 2);
  assert.equal(read(library + '/steamapps/common/NewPAL/game.exe'), 'original-game');
  assert.equal(read(library + '/steamapps/common/Steamworks Shared/redist.dat'), 'public-components');
  assert.equal(read(base + '/WindowsSteam/steamapps/common/unrelated.exe'), 'windows-original');
  assert.ok(fs.existsSync(library + '/steamapps/appmanifest_228980.acf'));
}
(async () => {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'steam-library-'));
  try {
    let base = temporary + '/normal', f = fixture(base);
    // Existing identical files and intersecting directories can be merged.
    f.file('client/.steam/steam/steamapps/common/NewPAL/shared.dat', 'same');
    f.file('library/steamapps/common/NewPAL/shared.dat', 'same');
    f.file('client/.steam/steam/steamapps/common/NewPAL/extra.dat', 'extra');
    const result = await helper.prepareSingleLinuxSteamLibrary(base);
    assert.equal(JSON.stringify(result.installedApps), '["228980","695630"]');
    check(base, f.steam, f.library);
    assert.equal((await helper.prepareSingleLinuxSteamLibrary(base)).movedEntries, 0);
    // A conflict discovered after a candidate move must leave every file intact.
    base = temporary + '/conflict'; f = fixture(base);
    f.file('client/.steam/steam/steamapps/common/NewPAL/game.exe', 'different-game');
    await assert.rejects(helper.prepareSingleLinuxSteamLibrary(base), /同名/);
    assert.equal(read(f.steam + '/steamapps/common/Steamworks Shared/redist.dat'), 'public-components');
    assert.equal(read(f.library + '/steamapps/common/NewPAL/game.exe'), 'original-game');
    assert.ok(!fs.existsSync(f.library + '/steamapps/.hmosdeck-single-library-v2'));
    // An operational rename failure is rolled back, then retry succeeds.
    base = temporary + '/failure'; f = fixture(base); moved = 0; failMove = 2;
    await assert.rejects(helper.prepareSingleLinuxSteamLibrary(base), /injected move/);
    assert.ok(fs.existsSync(f.steam + '/steamapps/appmanifest_228980.acf'));
    failMove = 0;
    await helper.prepareSingleLinuxSteamLibrary(base); check(base, f.steam, f.library);
    // A process interruption after moves/before activation is also resumable.
    base = temporary + '/config-failure'; f = fixture(base); failConfig = true;
    await assert.rejects(helper.prepareSingleLinuxSteamLibrary(base), /injected config/);
    assert.ok(!fs.existsSync(f.library + '/steamapps/.hmosdeck-single-library-v2'));
    failConfig = false;
    await helper.prepareSingleLinuxSteamLibrary(base); check(base, f.steam, f.library);
    // Do not silently remove a user-added, third-party library.
    base = temporary + '/extra'; f = fixture(base);
    fs.writeFileSync(f.steam + '/steamapps/libraryfolders.vdf', '"libraryfolders" { "0" { "path" "/other" } }');
    await assert.rejects(helper.prepareSingleLinuxSteamLibrary(base), /额外/);
    // Fresh clients produce a valid, non-scalar library entry with no games.
    base = temporary + '/empty';
    await helper.prepareSingleLinuxSteamLibrary(base);
    assert.equal(rules.parseSteamVdf(read(base + '/library/steamapps/libraryfolders.vdf')).child('libraryfolders').children.size, 1);
    console.log('PASS single library: both AppIDs, merge conflicts, rollback, interrupted retry, Windows isolation and idempotence');
  } finally { fs.rmSync(temporary, { recursive: true, force: true }); }
})().catch(error => { console.error(error); process.exitCode = 1; });
