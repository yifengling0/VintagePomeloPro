from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
(ROOT / 'build').mkdir(exist_ok=True)
sys.path.insert(0, str(ROOT / 'scripts'))
import verify_wine_overlay_stack as proof

def git(root, *args):
    return subprocess.run(['git', '-C', str(root), '-c', 'user.name=Overlay Test',
        '-c', 'user.email=test@example.invalid', *args], check=True, capture_output=True)

with tempfile.TemporaryDirectory(prefix='overlay-proof-test-', dir=ROOT / 'build') as temp:
    repo = Path(temp); (repo / 'build').mkdir(); (repo / 'scripts').mkdir()
    (repo / 'patches/wine').mkdir(parents=True)
    source = repo / 'source'; source.mkdir(); git(source, 'init', '-q')
    (source / 'unit.c').write_text('one\ntwo\nthree\n')
    git(source, 'add', '.'); git(source, 'commit', '-qm', 'baseline')
    a = repo / 'patches/wine/a.patch'; b = repo / 'patches/wine/b.patch'
    a.write_text('--- a/unit.c\n+++ b/unit.c\n@@ -1,3 +1,3 @@\n one\n-two\n+local\n three\n')
    b.write_text('--- a/unit.c\n+++ b/unit.c\n@@ -1,3 +1,3 @@\n one\n-local\n+combined\n three\n')
    (repo / 'scripts/build_wine.sh').write_text(
        'ensure_wine_patch "$ROOT/patches/wine/a.patch" "a"\n'
        'ensure_wine_patch "$ROOT/patches/wine/b.patch" "b"\n')
    # The normalizer deliberately invalidates reverse checking of a lower patch.
    def normalize(tree):
        p = tree / 'unit.c'; p.write_text(p.read_text().replace('three', 'normalized'))
    proof.wine_runtime_diagnostics.apply = normalize
    (source / 'unit.c').write_text('one\ncombined\nnormalized\n')
    before = (source / 'unit.c').read_bytes()
    proof.verify(source, repo, a)
    assert (source / 'unit.c').read_bytes() == before
    (source / 'unit.c').write_text('one\ncorrupt\nnormalized\n')
    try: proof.verify(source, repo, a)
    except ValueError as e: assert 'effective overlay differs' in str(e)
    else: raise AssertionError('corrupt overlay was accepted')
    assert 'corrupt' in (source / 'unit.c').read_text()
    unknown = repo / 'patches/wine/unknown.patch'; unknown.write_text(a.read_text())
    try: proof.verify(source, repo, unknown)
    except ValueError as e: assert 'not in the managed stack' in str(e)
    else: raise AssertionError('unregistered patch was accepted')
print('PASS overlay proof: overlapping patches, diagnostics, corruption rejection and source preservation')
