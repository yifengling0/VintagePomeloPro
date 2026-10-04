"""Execute unchanged ArkTS transaction logic with fault-injecting filesystem I/O."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    source = (ROOT / 'entry/src/main/ets/service/FontImportTransaction.ets').read_text()
    signature = """export async function replaceImportedFonts(files: FontImportFiles, source: string,
  destination: string, names: string[], token: string,
  progress: (done: number, total: number) => void): Promise<void> {"""
    # Erase only the known type/interface declarations, never rewrite executable
    # statements. A signature change deliberately requires updating this harness.
    assert source.count(signature) == 1
    body = source[source.index(signature):]
    javascript = body.replace(signature,
        'export async function replaceImportedFonts(files, source, destination, names, token, progress) {', 1)
    (work / 'transaction.mjs').write_text(javascript)
    (work / 'test.mjs').write_text(r'''
import assert from 'node:assert/strict';
import { replaceImportedFonts } from './transaction.mjs';

class Files {
  paths = new Map([['fonts', 'old'], ['source/new.ttf', 'new']]);
  fail = '';
  async exists(path) { return this.paths.has(path); }
  async mkdir(path) { assert(!this.paths.has(path)); this.paths.set(path, 'staged'); }
  async copy(source, destination) {
    if (this.fail === 'copy') throw Error('no space');
    this.paths.set(destination, this.paths.get(source));
  }
  async rename(source, destination) {
    if (this.fail === 'backup' && destination.includes('backup')) throw Error('backup failed');
    if (this.fail.startsWith('commit') && source.includes('import')) throw Error('commit failed');
    if (this.fail === 'commit+rollback' && source.includes('backup')) throw Error('rollback failed');
    assert(this.paths.has(source)); assert(!this.paths.has(destination));
    for (const [key, value] of [...this.paths]) {
      if (key === source || key.startsWith(source + '/')) {
        this.paths.set(destination + key.slice(source.length), value); this.paths.delete(key);
      }
    }
  }
  async remove(path) {
    assert(path.includes('.import-') || path.includes('.backup-'));
    for (const key of [...this.paths.keys()]) {
      if (key === path || key.startsWith(path + '/')) this.paths.delete(key);
    }
  }
}
for (const failure of ['', 'copy', 'backup', 'commit', 'commit+rollback']) {
  const files = new Files(); files.fail = failure;
  const operation = replaceImportedFonts(files, 'source', 'fonts', ['new.ttf'], '1', () => {});
  if (failure) await assert.rejects(operation); else await operation;
  if (!failure) assert.equal(files.paths.get('fonts/new.ttf'), 'new');
  else if (failure === 'commit+rollback') assert.equal(files.paths.get('fonts.backup-1'), 'old');
  else assert.equal(files.paths.get('fonts'), 'old');
  assert(!files.paths.has('fonts.import-1'));
  assert.equal(files.paths.get('source/new.ttf'), 'new');
}
for (const kind of ['import', 'backup']) {
  const files = new Files(); files.paths.set('fonts.' + kind + '-1', 'unowned');
  await assert.rejects(replaceImportedFonts(files, 'source', 'fonts', ['new.ttf'], '1', () => {}));
  assert.equal(files.paths.get('fonts'), 'old');
  assert.equal(files.paths.get('fonts.' + kind + '-1'), 'unowned');
}
const fresh = new Files(); fresh.paths.delete('fonts');
await replaceImportedFonts(fresh, 'source', 'fonts', ['new.ttf'], '1', () => {});
assert.equal(fresh.paths.get('fonts/new.ttf'), 'new');
console.log('font transaction: copy/backup/commit/rollback failures, collisions and fresh prefix passed');
''')
    sdk_node = Path(os.environ.get('TOOL_HOME', '/apps/harmony')) / 'tool/node/bin/node'
    node = os.environ.get('NODE') or shutil.which('node') or (str(sdk_node) if sdk_node.is_file() else None)
    if not node:
        raise SystemExit('font transaction tests need Node 18+; set NODE or TOOL_HOME to the SDK runtime')
    subprocess.run([node, str(work / 'test.mjs')], check=True)

# Cancellation never enters the replacement transaction.
page = (ROOT / 'entry/src/main/ets/pages/SystemSettings.ets').read_text()
confirm = page[page.index("title: '导入字体'"):page.index('private async applyFontImport')]
assert 'replaceImportedFonts' not in confirm
assert 'this.applyFontImport(tmpDir, cacheZip, fonts)' in confirm
cancel = confirm[confirm.index("value: '取消'"):confirm.index('secondaryButton:')]
assert 'applyFontImport' not in cancel

cleanup = page[page.index('private async removeFontImportDir'):page.index('private async applyFontImport')]
assert 'fs.lstatSync(path)' in cleanup and 'stat.isSymbolicLink()' in cleanup
assert 'fs.stat(' not in cleanup and 'fs.statSync(' not in cleanup
