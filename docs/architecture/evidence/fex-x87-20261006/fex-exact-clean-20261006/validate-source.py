from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import json

out = Path(__file__).resolve().parent
base = out.parent / 'fex-softfloat-lto-20261006/source'
current = out / 'source'
def hashes(root):
    return {str(p.relative_to(root)): hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
            for p in root.rglob('*') if p.is_file()}
a, b = hashes(base), hashes(current)
changed = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
expected = ['FEXCore/Source/Interface/Context/Context.h',
            'FEXCore/Source/Interface/Core/Core.cpp',
            'FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp']
assert changed == sorted(expected), changed
with tempfile.TemporaryDirectory(prefix='vp-float-replay-') as folder:
    root = Path(folder)
    for name in changed:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(current / name, target)
    with (out / 'fex-exact-store.patch').open('rb') as stream:
        subprocess.run(['patch', '-d', folder, '-p1', '-R', '-s'], stdin=stream, check=True)
    result = hashes(root)
    assert result == {k: a[k] for k in changed if k in a}, 'Reverse patch differs from exact baseline'
(out / 'source-validation.json').write_text(json.dumps(dict(changed=changed, filesVerified=len(b),
    exactBaselineAfterReverse=True), indent=2) + '\n')
print('EXACT SOURCE DELTA AND REVERSE REPLAY VERIFIED', len(b), 'files')
