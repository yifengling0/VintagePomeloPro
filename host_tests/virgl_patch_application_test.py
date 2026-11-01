"""Verify VirGL overlays from a clean or already-patched source export."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
args = parser.parse_args()
patches = sorted((ROOT / 'patches/virglrenderer').glob('*.patch'))
files = {name for overlay in patches
         for name in re.findall(r'^\+\+\+ b/(.+)$', overlay.read_text(), re.M)}
added = set()
for overlay in patches:
    text = overlay.read_text()
    sections = re.split(r'(?=^--- )', text, flags=re.M)
    for section in sections:
        old = re.search(r'^--- (.+)$', section, re.M)
        new = re.search(r'^\+\+\+ b/(.+)$', section, re.M)
        if old and old.group(1) == '/dev/null' and new:
            added.add(new.group(1))


def patch(source, overlay, reverse=False):
    subprocess.run(['patch', '-d', str(source), '-p1', '--force', '--batch',
                    '--fuzz=0', '--no-backup-if-mismatch'] + (['-R'] if reverse else []),
                   input=overlay.read_bytes(), check=True, stdout=subprocess.PIPE)


def snapshot(source):
    return {name: (source / name).read_bytes() if (source / name).is_file() else None
            for name in files}


with tempfile.TemporaryDirectory(prefix='virgl-patch-files-') as temporary:
    parent = Path(temporary)
    source = parent / 'export'
    baseline = {}
    for name in files:
        original = args.source / name
        if original.is_file():
            baseline[name] = original.read_bytes()
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(baseline[name])
        else:
            if name not in added:
                raise FileNotFoundError('pinned VirGL source file is missing: ' + name)
            baseline[name] = None
    subprocess.run(['git', 'init', '-q', str(parent)], check=True)
    assert snapshot(source) == baseline

    subprocess.run(['bash', str(ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh'),
                    str(source)], check=True, stdout=subprocess.PIPE)
    applied = snapshot(source)
    for name in files:
        if applied[name] is None:
            raise AssertionError('overlay failed to produce ' + name)
    subprocess.run(['bash', str(ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh'),
                    str(source)], check=True, stdout=subprocess.PIPE)
    assert snapshot(source) == applied, 'reapplying the VirGL overlay changed its output'

    for overlay in reversed(patches):
        patch(source, overlay, reverse=True)
    assert snapshot(source) == baseline, 'reversing the VirGL overlay did not restore its input'

    subprocess.run(['bash', str(ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh'),
                    str(source)], check=True, stdout=subprocess.PIPE)
    assert snapshot(source) == applied, 'overlay replay did not reproduce the same VirGL sources'
print('VirGL clean-source application, idempotence, reverse and replay PASS')
