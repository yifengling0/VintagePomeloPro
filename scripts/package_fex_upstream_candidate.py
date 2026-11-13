#!/usr/bin/env python3
"""Create an explicitly labelled FEX component-test HAP from a verified base.

This does not promote production pins or weaken the release source-build gate.
Every unchanged member is compared by SHA-256, including the nested runtime.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
from zipfile import ZipFile

from prepare_fex_upstream import ROOT, LOCK
from fex_simd_runtime import MEMBERS, pe_kind, validate
from source_build_contract import elf_identity

PAYLOAD = 'resources/rawfile/wine-data.zip'
MANIFEST = 'resources/rawfile/wine-runtime-manifest.json'
VERSION = 'resources/rawfile/wine_runtime.version'
RECORD = 'fex-upstream-candidate.json'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with path.open('rb') as stream:
        return stream_sha(stream)


def stream_sha(stream):
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(chunk)
    return digest.hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2) + '\n').encode()


def rewrite(source, output, changes, additions):
    names = source.namelist()
    if len(names) != len(set(names)) or not set(changes) <= set(names) or set(additions) & set(names):
        raise ValueError('Ambiguous or unexpected ZIP inventory')
    with ZipFile(output, 'w', allowZip64=True) as target:
        for item in source.infolist():
            value = changes.get(item.filename)
            if isinstance(value, bytes):
                target.writestr(copy.copy(item), value)
            else:
                with target.open(copy.copy(item), 'w', force_zip64=True) as dest:
                    if value is None:
                        with source.open(item) as stream:
                            shutil.copyfileobj(stream, dest, 1024 * 1024)
                    else:
                        with value.open('rb') as stream:
                            shutil.copyfileobj(stream, dest, 1024 * 1024)
        for name, data in additions.items():
            target.writestr(name, data)


def audit(before, after, changes, additions):
    if set(after.namelist()) != set(before.namelist()) | set(additions):
        raise ValueError('Candidate inventory changed outside the component override')
    differences = []
    for name in before.namelist():
        with before.open(name) as stream:
            old = stream_sha(stream)
        with after.open(name) as stream:
            new = stream_sha(stream)
        if old != new:
            differences.append(name)
    if set(differences) != set(changes):
        raise ValueError('Unexpected candidate member changes: ' + repr(differences))
    return differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--base-receipt', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() == args.baseline.resolve() or args.output.exists():
        raise ValueError('Inspect and reuse an existing candidate; never overwrite the base')
    base = json.loads(args.base_receipt.read_text())
    if args.baseline.stat().st_size != base['hap']['bytes'] or file_sha(args.baseline) != base['hap']['sha256']:
        raise ValueError('Base HAP differs from its verified build receipt')
    lock = json.loads(LOCK.read_text())
    build_receipt = args.build / 'fex-upstream-build.json'
    build = json.loads(build_receipt.read_text())
    if build['lockSha256'] != file_sha(LOCK) or build['adaptedTree'] != lock['adaptedTree']:
        raise ValueError('Component build differs from the migration lock')
    staged = args.build / 'staged'
    module_data = {}
    for filename, item in build['modules'].items():
        data = (staged / filename).read_bytes()
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('Component build checksum mismatch: ' + filename)
        module_data[filename] = data
    for key, filename in (('arm64ec', 'libarm64ecfex.dll'), ('wow64', 'libwow64fex.dll')):
        data = module_data[filename]
        if pe_kind(data) != ('arm64ec' if key == 'arm64ec' else 'arm64') or data[64:80] != b'Wine builtin DLL':
            raise ValueError('FEX module cannot participate in the Wine Unix bridge')
    if (staged / 'libwow64fex.so').read_bytes() != module_data['libarm64ecfex.so']:
        raise ValueError('WOW64 Unix bridge alias does not match the paired build')
    override = {'schemaVersion': 1, 'policy': 'component-test-candidate', 'deviceValidated': False,
                'baseSourceCommit': base['sourceCommit'], 'baseHapSha256': base['hap']['sha256'],
                'lockSha256': file_sha(LOCK), 'upstreamCommit': lock['upstream']['commit'],
                'adaptedTree': lock['adaptedTree'], 'adaptedCommit': lock['adaptedCommit'],
                'buildReceiptSha256': file_sha(build_receipt), 'modules': build['modules'],
                'productionPinsChanged': False,
                'unixBridgeLoader': 'Wine builtin PE marker + MemoryWineUnixFuncs + OHOS library namespace'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fex-package-', dir=args.output.parent) as directory:
        scratch = Path(directory)
        original, updated = scratch / 'original.zip', scratch / 'updated.zip'
        with ZipFile(args.baseline) as hap:
            with hap.open(PAYLOAD) as stream, original.open('wb') as dest:
                shutil.copyfileobj(stream, dest, 1024 * 1024)
            manifest = json.loads(hap.read(MANIFEST))
            if manifest['payloadSha256'] != file_sha(original) or manifest['sourceBuild']['commit'] != base['sourceCommit']:
                raise ValueError('Base runtime does not match the verified base')
            with ZipFile(original) as runtime:
                fex = json.loads(runtime.read('fex-simd-zero.json'))
                validate(fex, runtime.read)
                fex['sourceHash'] = lock['adaptedCommit']
                fex['migration'] = {'lockSha256': file_sha(LOCK), 'upstreamCommit': lock['upstream']['commit'],
                                    'adaptedTree': lock['adaptedTree'], 'patches': lock['patches']}
                # Keep the established repair identity and state how this source implements it.
                fex['fixes']['smcBlockState']['implementation'] = 'included-in-upstream'
                changes = {MEMBERS['arm64ec']: module_data['libarm64ecfex.dll'],
                           MEMBERS['wow64']: module_data['libwow64fex.dll']}
                for key, name in MEMBERS.items():
                    fex['modules'][key]['sha256'] = sha(changes[name])
                receipt = json.loads(runtime.read('source-build-candidate.json'))
                receipt['policy'] = 'component-test-candidate'
                receipt['componentOverrides'] = {'fex': override}
                bridge = module_data['libarm64ecfex.so']
                for name in ('libarm64ecfex.so', 'libwow64fex.so'):
                    receipt['libraries']['libs/arm64-v8a/' + name] = {
                        'sha256': sha(bridge), 'bytes': len(bridge), 'loadableSha256': elf_identity(bridge)}
                for name, data in changes.items():
                    receipt['payload'][name] = {'sha256': sha(data), 'bytes': len(data)}
                changes['fex-simd-zero.json'] = encoded(fex)
                changes['source-build-candidate.json'] = encoded(receipt)
                additions = {RECORD: encoded(override)}
                rewrite(runtime, updated, changes, additions)
                with ZipFile(updated) as candidate:
                    inner_changes = audit(runtime, candidate, changes, additions)
                    validate(fex, candidate.read)
            manifest['payloadSha256'] = file_sha(updated)
            manifest['validationPolicy'] = 'component-test-candidate'
            manifest['componentOverrides'] = {'fex': override}
            version, count = re.subn(rb'content=[^;\s]+', b'content=' + file_sha(updated)[:16].encode(), hap.read(VERSION))
            if count != 1:
                raise ValueError('Ambiguous runtime content identity')
            version = version.rstrip() + b';fex-candidate=' + lock['adaptedCommit'][:10].encode() + b'\n'
            outer_changes = {PAYLOAD: updated, MANIFEST: encoded(manifest), VERSION: version}
            native = {'libs/arm64-v8a/' + name: module_data['libarm64ecfex.so']
                      for name in ('libarm64ecfex.so', 'libwow64fex.so')}
            rewrite(hap, args.output, outer_changes, native)
            with ZipFile(args.output) as candidate:
                hap_changes = audit(hap, candidate, outer_changes, native)
                for name, data in native.items():
                    if candidate.read(name) != data:
                        raise ValueError('Missing matching Unix bridge')
        result = dict(override, candidate=str(args.output), unsignedSha256=file_sha(args.output),
                      unsignedBytes=args.output.stat().st_size, changedHapMembers=hap_changes,
                      addedHapMembers=list(native), changedRuntimeMembers=inner_changes,
                      addedRuntimeMembers=[RECORD], unchangedMembersSha256Verified=True)
        args.output.with_suffix('.json').write_bytes(encoded(result))
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
