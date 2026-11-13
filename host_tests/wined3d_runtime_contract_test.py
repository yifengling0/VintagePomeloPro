"""Exercise the package gate with the real old, regressed and repaired DLLs."""
import argparse
import copy
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from wined3d_runtime_contract import MODULES, SOURCE_FILES, sha, source_contract, validate_module, validate_runtime

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-hap', required=True, type=Path)
parser.add_argument('--regressed-hap', required=True, type=Path)
parser.add_argument('--modules-dir', required=True, type=Path)
parser.add_argument('--wine-source', required=True, type=Path)
args = parser.parse_args()


def rejected(call, reason):
    try:
        call()
    except ValueError as error:
        assert reason in str(error), error
    else:
        raise AssertionError('accepted invalid runtime: ' + reason)


for path, reason in [(args.baseline_hap, 'obsolete thread-ID'),
                     (args.regressed_hap, 'missing WineD3D fix')]:
    with ZipFile(path) as hap, ZipFile(io.BytesIO(hap.read('resources/rawfile/wine-data.zip'))) as wine:
        for arch in MODULES:
            data = wine.read(f'bin/{arch}/wined3d.dll')
            rejected(lambda: validate_module(data, arch), reason)

record = {'version': 1, **source_contract(args.wine_source), 'files': []}
modules = {}
for arch in MODULES:
    member = f'bin/{arch}/wined3d.dll'
    modules[member] = data = (args.modules_dir / arch / 'wined3d.dll').read_bytes()
    record['files'].append({'path': member, 'bytes': len(data), 'sha256': sha(data)})
validate_runtime(record, modules.__getitem__)
for name, reason, change in [
    ('stale DLL', 'DLL identity mismatch', lambda r: r['files'][0].update(sha256='0'*64)),
    ('missing architecture', 'architecture inventory', lambda r: r['files'].pop()),
    ('missing overlay', 'overlay inventory', lambda r: r['patches'].pop(next(iter(r['patches'])))),
    ('missing source', 'source identities', lambda r: r['sourceFiles'].clear()),
]:
    bad = copy.deepcopy(record)
    change(bad)
    rejected(lambda: validate_runtime(bad, modules.__getitem__), reason)

# A source snapshot can be nested inside the parent Git repository. Verify
# patch presence against its files, rather than Git's path filtering.
with tempfile.TemporaryDirectory(prefix='wined3d-contract-') as temporary:
    snapshot = Path(temporary)
    directory = snapshot / 'dlls/wined3d'
    directory.mkdir(parents=True)
    for name in SOURCE_FILES:
        shutil.copyfile(args.wine_source / 'dlls/wined3d' / name, directory / name)
    source_contract(snapshot)
    patch = ROOT / 'patches/wine/0053-wined3d-restore-legacy-alpha-test-state.patch'
    subprocess.run(['patch', '-d', str(snapshot), '-p1', '--force', '--batch', '--reverse',
                    '-i', str(patch)], check=True, capture_output=True)
    rejected(lambda: source_contract(snapshot), '0053-wined3d')

print('PASS: repaired pair accepted; old queue backend, queue-only regression, stale DLL, '
      'missing architecture/overlay/source and missing effective alpha patch rejected')
