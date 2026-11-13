#!/usr/bin/env python3
"""Exercise managed display overlays on real clean, patched and dirty Git trees."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ROOT.joinpath('build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='display-stack-test-', dir=ROOT / 'build') as directory:
    work = Path(directory)
    source = work / 'source'
    source.mkdir()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args])
    git('init', '-q')
    git('config', 'user.name', 'test')
    git('config', 'user.email', 'test@example.invalid')
    (source / 'file').write_text('base\n')
    git('add', 'file')
    git('commit', '-qm', 'base')
    patches = []
    for index, (before, after) in enumerate([('base', 'first'), ('first', 'final')]):
        p = work / (str(index) + '.patch')
        p.write_text('--- a/file\n+++ b/file\n@@ -1 +1 @@\n-' + before + '\n+' + after + '\n')
        patches.append(str(p))
    command = ['python3', str(ROOT / 'scripts/apply_managed_display_overlays.py'),
               '--source', str(source), '--scratch', str(work), *patches]
    index = git('ls-files', '--stage')
    subprocess.run(command, check=True)
    assert (source / 'file').read_text() == 'final\n'
    subprocess.run(command, check=True)
    assert git('ls-files', '--stage') == index
    (source / 'file').write_text('user change\n')
    result = subprocess.run(command, capture_output=True)
    assert result.returncode != 0 and b'unrecognized local changes preserved' in result.stderr
    assert (source / 'file').read_text() == 'user change\n'
    assert git('ls-files', '--stage') == index
print('PASS display overlay stack: ordered replay, idempotence, dirty-source preservation, unchanged index')
