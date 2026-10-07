// Exercise the production ArkTS file operations with a real host filesystem.
const assert = require('node:assert/strict');
const nodefs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const ts = require(process.argv[2]);
const source = nodefs.readFileSync(path.resolve(__dirname,
  '../entry/src/main/ets/service/WineEngineService.ets'), 'utf8');
const methods = source.slice(source.indexOf('  private binaryFilesMatch('),
  source.indexOf('  private async ensureRuntimeInstalled('));
const base = nodefs.mkdtempSync(path.join(os.tmpdir(), 'vp-managed-cpu-'));
const WINE_BIN = base + '/wine/bin';
const PREFIX_DIR = base + '/.wine';
const pathExists = p => nodefs.existsSync(p);
const hilog = { info() {} };
const DOMAIN = 0, TAG = 'test';
let copies = 0, failCopy = false, failRename = false, shortRead = false;
const fs = {
  OpenMode: { READ_ONLY: 'r' },
  statSync: nodefs.statSync,
  mkdirSync: (name, recursive) => nodefs.mkdirSync(name, { recursive }),
  openSync: (name, mode) => ({ fd: nodefs.openSync(name, mode) }),
  closeSync: file => nodefs.closeSync(file.fd),
  readSync(fd, buffer, options) {
    return nodefs.readSync(fd, Buffer.from(buffer), 0,
      options.length - (shortRead ? 1 : 0), options.offset);
  },
  copyFileSync(from, to) {
    ++copies;
    if (failCopy) nodefs.writeFileSync(to, 'incomplete');
    else nodefs.copyFileSync(from, to);
  },
  renameSync(from, to) {
    if (failRename) throw new Error('rename failed');
    nodefs.renameSync(from, to);
  },
  unlinkSync: nodefs.unlinkSync
};
eval(ts.transpileModule('class ManagedCPU {' + methods + '}\nglobalThis.ManagedCPU = ManagedCPU;',
  { compilerOptions: { target: ts.ScriptTarget.ES2020 } }).outputText);
const service = new globalThis.ManagedCPU();
const system32 = PREFIX_DIR + '/drive_c/windows/system32';
const native = WINE_BIN + '/aarch64-windows';
const wow = 'libwow64fex.dll', ec = 'libarm64ecfex.dll';
try {
  service.syncManagedFexModules();
  assert.equal(copies, 0, 'Historical x64-only runtime needs no FEX');
  assert.equal(pathExists(system32), false, 'Historical runtime creates no FEX skeleton');
  nodefs.mkdirSync(native, { recursive: true });
  const old = Buffer.alloc(131083, 17), fixed = Buffer.alloc(131083, 18);
  nodefs.writeFileSync(native + '/' + wow, fixed);
  assert.throws(() => service.syncManagedFexModules(), /Missing or empty managed CPU module/);
  assert.equal(pathExists(system32), false, 'Reject an incomplete pair before touching prefix');
  nodefs.writeFileSync(native + '/' + ec, '');
  assert.throws(() => service.syncManagedFexModules(), /Missing or empty managed CPU module/);
  nodefs.writeFileSync(native + '/' + ec, fixed);
  service.syncManagedFexModules();
  assert.equal(copies, 2, 'Fresh prefix deploys both CPU DLLs before wineboot');
  for (const name of [wow, ec]) assert.deepEqual(nodefs.readFileSync(system32 + '/' + name), fixed);
  copies = 0;
  for (const name of [wow, ec]) {
    nodefs.writeFileSync(native + '/' + name, fixed);
    nodefs.writeFileSync(system32 + '/' + name, old);
  }
  nodefs.writeFileSync(system32 + '/custom-game.dll', 'user override');
  service.syncManagedFexModules();
  assert.equal(copies, 2);
  for (const name of [wow, ec]) assert.deepEqual(nodefs.readFileSync(system32 + '/' + name), fixed);
  assert.equal(nodefs.readFileSync(system32 + '/custom-game.dll', 'utf8'), 'user override');
  service.syncManagedFexModules();
  assert.equal(copies, 2, 'Unchanged cold start does not rewrite modules');
  nodefs.writeFileSync(system32 + '/' + wow, old);
  shortRead = true;
  assert.throws(() => service.syncManagedFexModules(), /Incomplete/);
  assert.deepEqual(nodefs.readFileSync(system32 + '/' + wow), old);
  shortRead = false;
  failCopy = true;
  assert.throws(() => service.syncManagedFexModules(), /Incomplete managed CPU module copy/);
  assert.deepEqual(nodefs.readFileSync(system32 + '/' + wow), old);
  assert.equal(pathExists(system32 + '/' + wow + '.winehua-update'), false);
  failCopy = false; failRename = true;
  assert.throws(() => service.syncManagedFexModules(), /rename failed/);
  assert.deepEqual(nodefs.readFileSync(system32 + '/' + wow), old);
  assert.equal(pathExists(system32 + '/' + wow + '.winehua-update'), false);
  failRename = false;
  service.syncManagedFexModules();
  assert.deepEqual(nodefs.readFileSync(system32 + '/' + wow), fixed);
  nodefs.unlinkSync(system32 + '/' + ec);
  service.syncManagedFexModules();
  assert.deepEqual(nodefs.readFileSync(system32 + '/' + ec), fixed);
  console.log('PASS: fresh-prefix 32/64 CPU deployment, incomplete/empty pair rejection, upgrades, unchanged files, user DLL preservation, short reads, partial copies, rename failure and retry');
} finally {
  nodefs.rmSync(base, { recursive: true, force: true });
}
