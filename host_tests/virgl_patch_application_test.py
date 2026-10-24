"""Check real files, including a source export nested in another Git repo."""
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
files = {name for patch in patches
         for name in re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M)}


def patch(source, overlay, reverse=False):
    subprocess.run(['patch', '-d', str(source), '-p1', '--force', '--batch',
                    '--fuzz=0', '--no-backup-if-mismatch'] + (['-R'] if reverse else []),
                   input=overlay.read_bytes(), check=True, stdout=subprocess.PIPE)


with tempfile.TemporaryDirectory(prefix='virgl-patch-files-') as temporary:
    parent = Path(temporary)
    source = parent / 'export'
    expected = {}
    for name in files:
        expected[name] = (args.source / name).read_bytes()
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(expected[name])
    subprocess.run(['git', 'init', '-q', str(parent)], check=True)
    for overlay in reversed(patches):
        patch(source, overlay, reverse=True)
    for iteration in range(2):
        subprocess.run(['bash', str(ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh'),
                        str(source)], check=True, stdout=subprocess.PIPE)
        for name, data in expected.items():
            assert (source / name).read_bytes() == data, (iteration, name)
    patch(source, patches[-1], reverse=True)
    assert (source / 'vtest/vtest_renderer.c').read_bytes() != expected['vtest/vtest_renderer.c']
    subprocess.run(['bash', str(ROOT / 'scripts/apply_virglrenderer_ohos_patches.sh'),
                    str(source)], check=True, stdout=subprocess.PIPE)
    assert (source / 'vtest/vtest_renderer.c').read_bytes() == expected['vtest/vtest_renderer.c']
print('VirGL clean export, idempotence and missing producer-context overlay PASS')
