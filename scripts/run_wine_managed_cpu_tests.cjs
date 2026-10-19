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
const WINE_ROOT = base + '/wine';
const WINE_BIN = WINE_ROOT + '/bin';
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
  const beforeIcu = copies;
  service.syncManagedIcuModules();
  assert.equal(copies, beforeIcu, 'Historical payload without ICU record remains supported');
  nodefs.writeFileSync(WINE_ROOT + '/icu-compat.json', '{}');
  assert.throws(() => service.syncManagedIcuModules(), /managed ICU module/);
  const builtin = Buffer.alloc(150123, 20);
  builtin.write('Wine builtin DLL', 64, 'ascii');
  const builtinOld = Buffer.from(builtin); builtinOld[1000] = 21;
  const icuNames = ['icu.dll', 'icuuc.dll', 'icuin.dll'];
  for (const arch of ['aarch64-windows', 'i386-windows']) {
    nodefs.mkdirSync(WINE_BIN + '/' + arch, { recursive: true });
    for (const name of icuNames) nodefs.writeFileSync(WINE_BIN + '/' + arch + '/' + name, builtin);
  }
  assert.throws(() => service.syncManagedIcuModules(), /Unicode data/);
  nodefs.mkdirSync(WINE_ROOT + '/share/wine/nls', { recursive: true });
  nodefs.writeFileSync(WINE_ROOT + '/share/wine/nls/icudtl.dat', 'Unicode data');
  nodefs.writeFileSync(system32 + '/icu.dll', builtinOld);
  nodefs.writeFileSync(system32 + '/icuuc.dll', 'user installed native ICU override');
  service.syncManagedIcuModules();
  assert.deepEqual(nodefs.readFileSync(system32 + '/icu.dll'), builtin);
  assert.equal(nodefs.readFileSync(system32 + '/icuuc.dll', 'utf8'), 'user installed native ICU override');
  const syswow64 = PREFIX_DIR + '/drive_c/windows/syswow64';
  for (const name of icuNames) assert.deepEqual(nodefs.readFileSync(syswow64 + '/' + name), builtin);
  const unchanged = copies; service.syncManagedIcuModules();
  assert.equal(copies, unchanged, 'Same ICU does not rewrite prefix DLLs');
  nodefs.writeFileSync(syswow64 + '/icu.dll', builtinOld);
  failCopy = true;
  assert.throws(() => service.syncManagedIcuModules(), /Incomplete/);
  assert.deepEqual(nodefs.readFileSync(syswow64 + '/icu.dll'), builtinOld);
  assert.equal(pathExists(syswow64 + '/icu.dll.winehua-update'), false);
  failCopy = false; failRename = true;
  assert.throws(() => service.syncManagedIcuModules(), /rename failed/);
  assert.deepEqual(nodefs.readFileSync(syswow64 + '/icu.dll'), builtinOld);
  failRename = false; service.syncManagedIcuModules();
  assert.deepEqual(nodefs.readFileSync(syswow64 + '/icu.dll'), builtin);
  console.log('PASS: paired CPU upgrade; generic ICU upgrade; 32/64 builtins; native overrides; incomplete data; copy/rename failure and retry');
} finally {
  nodefs.rmSync(base, { recursive: true, force: true });
}
