// Run production retention against real files; protect profiles and altered data.
const fs = require('node:fs'), path = require('node:path'), os = require('node:os');
const crypto = require('node:crypto'), vm = require('node:vm'), assert = require('node:assert/strict');
const ts = require(process.argv[2] || 'typescript');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'steam-retention-'));
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const config = {
  read: (file, limit) => { const data = fs.readFileSync(file); assert(data.length <= limit); return data; },
  text: data => Buffer.from(data).toString('utf8'), hash: async data => hash(data),
  fileHash: async file => hash(fs.readFileSync(file))
};
const mocks = { '@kit.CoreFileKit': { fileIo: {
  accessSync: fs.existsSync, listFileSync: fs.readdirSync, lstatSync: fs.lstatSync,
  statSync: fs.statSync, unlinkSync: fs.unlinkSync
}}, './ConfigurationFiles': { ConfigurationFiles: config }};
const source = path.resolve(__dirname, '../entry/src/main/ets/service/LinuxSteamReleaseRetention.ets');
const compiled = ts.transpileModule(fs.readFileSync(source, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021 }
}).outputText;
const moduleResult = { exports: {} };
vm.runInNewContext('(function(require,module,exports){' + compiled + '\n})', {})(
  name => { assert(name in mocks, name); return mocks[name]; }, moduleResult, moduleResult.exports);
const reclaim = moduleResult.exports.reclaimLinuxSteamReleases;
function put(file, data) { fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, data); }
function release(root, label, timestamp, override = null) {
  const content = Buffer.from('native-code-' + label);
  const manifest = { schemaVersion: 1, layout: 'SteamLinuxARM64', executionModel: 'native_aarch64',
    files: [{ path: 'runtime/library.so', sha256: hash(content), bytes: content.length }] };
  if (override) override(manifest);
  const text = JSON.stringify(manifest), version = hash(text), directory = path.join(root, 'releases', version);
  put(directory + '/runtime/library.so', content);
  put(directory + '/external-manifest.json', text); put(directory + '/.verified', version);
  fs.utimesSync(directory + '/external-manifest.json', timestamp, timestamp);
  return { version, directory, file: directory + '/runtime/library.so' };
}
(async () => {
  const root = temporary + '/SteamLinuxARM64';
  const active = release(root, 'active', 30), pending = release(root, 'pending', 40);
  const rollback = release(root, 'rollback', 20), old = release(root, 'old', 10);
  const changed = release(root, 'changed', 11); fs.writeFileSync(changed.file, 'user-modified-content');
  const escape = release(root, 'escape', 12, manifest => { manifest.files[0].path = 'runtime/../../client/save'; });
  const tampered = release(root, 'tampered', 13); fs.appendFileSync(tampered.directory + '/external-manifest.json', ' ');
  const linked = release(root, 'linked', 14), outside = temporary + '/outside';
  put(outside, 'outside'); fs.unlinkSync(linked.file); fs.symlinkSync(outside, linked.file);
  const shared = release(root, 'symlink-parent', 15), sharedFile = temporary + '/shared/library.so';
  put(sharedFile, fs.readFileSync(shared.file)); fs.unlinkSync(shared.file); fs.rmdirSync(path.dirname(shared.file));
  fs.symlinkSync(path.dirname(sharedFile), path.dirname(shared.file));
  put(root + '/client/save', 'precious-save'); put(old.directory + '/runtime/unrecognized', 'custom');
  const result = await reclaim(root, active.version, pending.version, true);
  assert(result.removedFiles >= 1); assert(!fs.existsSync(old.file));
  assert(!fs.existsSync(old.directory + '/.verified'));
  for (const item of [active, pending, rollback, changed, escape, tampered, linked]) assert(fs.existsSync(item.file), item.directory);
  assert(fs.existsSync(sharedFile), 'symlink parent must not redirect cleanup outside a release');
  assert.equal(fs.readFileSync(root + '/client/save', 'utf8'), 'precious-save');
  assert.equal(fs.readFileSync(old.directory + '/runtime/unrecognized', 'utf8'), 'custom');
  await reclaim(root, pending.version, active.version, false);
  assert(!fs.existsSync(rollback.file)); assert(fs.existsSync(active.file)); assert(fs.existsSync(pending.file));
  console.log('PASS active/pending/rollback retention; real reclamation; checksums, path traversal, symlinks, unknown files and saves protected');
})().then(() => fs.rmSync(temporary, { recursive: true, force: true })).catch(error => {
  console.error(error); fs.rmSync(temporary, { recursive: true, force: true }); process.exitCode = 1;
});
