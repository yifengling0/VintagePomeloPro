"""The packaged FEX identity follows final PE bytes after symbol stripping."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import fex_simd_runtime as fex
from stamp_wine_builtin_identity import stamp, validate_archive


def pe(machine):
    data = bytearray(256)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, machine, 0)
    struct.pack_into('<H', data, 0x94, 0)
    struct.pack_into('<H', data, 0x98, 0x10b)
    return bytes(data)


def digest(data):
    return hashlib.sha256(data).hexdigest()


with tempfile.TemporaryDirectory(prefix='fex-simd-runtime-') as temporary:
    directory = Path(temporary)
    modules = {
        'arm64ec': pe(0xa641),
        'wow64': pe(0xaa64),
    }
    for key, name in fex.MEMBERS.items():
        target = directory / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(modules[key])
    record = {
        'schemaVersion': 1,
        'repairVersion': 1,
        'sourceHash': 'fixture-source',
        'patchSha256': 'a' * 64,
        'modules': {
            key: {'member': name, 'sha256': digest(modules[key])}
            for key, name in fex.MEMBERS.items()
        },
    }
    (directory / fex.RECORD).write_text(json.dumps(record, indent=2) + '\n')
    fex.validate(record, lambda name: (directory / name).read_bytes())

    # Model the assembly-wide llvm-strip pass changing the arm64ec module.
    arm64ec = directory / fex.MEMBERS['arm64ec']
    stripped = arm64ec.read_bytes() + b'packaging-strip-marker'
    arm64ec.write_bytes(stripped)
    try:
        fex.validate(record, lambda name: (directory / name).read_bytes())
    except ValueError as error:
        assert 'checksum mismatch: arm64ec' in str(error)
    else:
        raise AssertionError('pre-transform record unexpectedly accepted stripped FEX')

    subprocess.run([
        sys.executable, str(ROOT / 'scripts/fex_simd_runtime.py'),
        'refresh-staged', '--directory', str(directory),
    ], check=True)
    refreshed = json.loads((directory / fex.RECORD).read_text())
    assert refreshed['sourceHash'] == 'fixture-source'
    assert refreshed['patchSha256'] == 'a' * 64
    assert refreshed['modules']['arm64ec']['sha256'] == digest(stripped)
    fex.validate(refreshed, lambda name: (directory / name).read_bytes())

    # Historical records remain readable, but must not silently become a new
    # release without the generic SMC fix. This also catches old build caches.
    try:
        fex.require_current(refreshed)
    except ValueError as error:
        assert 'predates required SMC repair' in str(error)
    else:
        raise AssertionError('legacy FEX repair accepted for new packaging')

    current = json.loads(json.dumps(refreshed))
    current['repairVersion'] = fex.REPAIR_VERSION
    current['fixes'] = {'smcBlockState': {'upstreamCommit': fex.SMC_COMMIT,
        'patchSha256': digest(fex.SMC_PATCH.read_bytes())}}
    fex.validate(current, lambda name: (directory / name).read_bytes())
    fex.require_current(current)
    (directory / fex.RECORD).write_text(json.dumps(current))
    fex.refresh_staged(directory)
    assert json.loads((directory / fex.RECORD).read_text())['fixes'] == current['fixes']
    # Targeted FEX delivery must also refresh Wineboot's builtin identity,
    # otherwise an existing prefix can keep stale PE modules after extraction.
    build, raw = directory / 'build', directory / 'raw'
    raw.mkdir()
    newer = {}
    for key, name in fex.MEMBERS.items():
        data = (directory / name).read_bytes() + b'new-fex-build'
        newer[name] = data
        target = build / ('fex-ec' if key == 'arm64ec' else 'fex-pe') / 'Bin' / Path(name).name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    delivery_record = json.loads(json.dumps(current))
    for key, name in fex.MEMBERS.items():
        delivery_record['modules'][key]['sha256'] = digest(newer[name])
    (build / fex.RECORD).write_text(json.dumps(delivery_record))
    old_inf = stamp(b'; fixture Wine INF\n', [(name, digest((directory / name).read_bytes()))
                                               for name in fex.MEMBERS.values()])
    with ZipFile(raw / 'wine-data.zip', 'w') as archive:
        for name in fex.MEMBERS.values():
            archive.writestr(name, (directory / name).read_bytes())
        archive.writestr('share/wine/wine.inf', old_inf)
        archive.writestr('preserved.txt', b'unchanged runtime data')
    (raw / 'wine-runtime-manifest.json').write_text(json.dumps({
        'payloadSha256': digest((raw / 'wine-data.zip').read_bytes())}))
    (raw / 'wine_runtime.version').write_text('content=old;wine=fixture\n')
    fex.update_rawfile(build, raw)
    with ZipFile(raw / 'wine-data.zip') as archive:
        validate_archive(archive)
        assert archive.read('share/wine/wine.inf') != old_inf
        assert archive.read('preserved.txt') == b'unchanged runtime data'
        for name, data in newer.items():
            assert archive.read(name) == data
        fex.validate(json.loads(archive.read(fex.RECORD)), archive.read)

    current['fixes']['smcBlockState']['patchSha256'] = '0' * 64
    try:
        fex.require_current(current)
    except ValueError as error:
        assert 'patch changed' in str(error)
    else:
        raise AssertionError('stale SMC patch hash accepted')
    del current['fixes']['smcBlockState']
    try:
        fex.validate(current, lambda name: (directory / name).read_bytes())
    except ValueError as error:
        assert 'SMC block-state repair identity' in str(error)
    else:
        raise AssertionError('missing required fix accepted')

print('FEX staged identity follows final transformed module bytes PASS')
