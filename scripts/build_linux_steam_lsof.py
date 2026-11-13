#!/usr/bin/env python3
"""Build a marked GNU lsof with an opt-in live socket backend for OHOS."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from linux_component_identity import compiler_identity

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = 'e9faa0fbcc48638c1d1f143e93573ac43b65e76646150f83e24bd8c18786303c'
PATCH = ROOT / 'patches/lsof/0001-ohos-live-socket-query-backend.patch'
REGISTRY_PATCH = ROOT / 'patches/lsof/0002-cooperative-tcp-registry-scan.patch'
HEADER = ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture/lsof_socket_backend.h'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--cc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--native-host-test', action='store_true')
    args = parser.parse_args()
    if sha(args.archive) != SOURCE_SHA256:
        parser.error('Expected the pinned lsof 4.95.0 source archive')
    if not 1 <= args.jobs <= 32:
        parser.error('Use 1-32 jobs')
    cc = args.cc.resolve(strict=True)
    target = subprocess.check_output([str(cc), '-dumpmachine'], text=True).strip()
    if ('linux-gnu' not in target or
        (args.native_host_test and not target.startswith(os.uname().machine + '-')) or
        (not args.native_host_test and not target.startswith('aarch64'))):
        parser.error('Expected a GNU AArch64 compiler, or an explicit x86_64 host test')
    output = args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    inputs=dict(source=SOURCE_SHA256,compiler=compiler_identity(cc),
        sources={str(p.relative_to(ROOT)):sha(p) for p in [Path(__file__).resolve(),PATCH,REGISTRY_PATCH,HEADER,HEADER.with_name('socket_query_protocol.h'),ROOT/'scripts/linux_component_identity.py']})
    receipt=output/'manifest.json'
    old=json.loads(receipt.read_text()) if receipt.exists() else {}
    binary=output/'steamohos_lsof'
    if old.get('buildInputs')==inputs and binary.exists() and old.get('files',{}).get(binary.name,{}).get('sha256')==sha(binary):
        print('Reused matching native lsof');return
    with tarfile.open(args.archive) as archive:
        for entry in archive:
            destination = (output / entry.name).resolve()
            if not destination.is_relative_to(output):
                parser.error('Source archive path escapes output')
            if not entry.isfile():
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.extractfile(entry).read())
            destination.chmod(entry.mode & 0o777)
    source = output / 'lsof_4.95.0.linux'
    with PATCH.open('rb') as stream:
        subprocess.run(['patch', '-p1', '--fuzz=0', '--batch', '--forward'],
                       cwd=source, stdin=stream, check=True)
    with REGISTRY_PATCH.open('rb') as stream:
        subprocess.run(['patch','-p1','--fuzz=0','--batch','--forward'],cwd=source,stdin=stream,check=True)
    shutil.copyfile(HEADER, source / HEADER.name)
    shutil.copyfile(HEADER.with_name('socket_query_protocol.h'),source/'socket_query_protocol.h')
    sysroot = subprocess.check_output([str(cc), '-print-sysroot'], text=True).strip()
    include = Path(sysroot or '/') / 'usr/include'
    if not args.native_host_test and sysroot in ('','/') and (Path('/usr')/target/'include').is_dir():
        include=Path('/usr')/target/'include'
    environment = os.environ.copy()
    ar = subprocess.check_output([str(cc), '-print-prog-name=ar'], text=True).strip()
    ranlib = subprocess.check_output([str(cc), '-print-prog-name=ranlib'], text=True).strip()
    defines = subprocess.check_output([str(cc), '-dM', '-E', '-include', 'features.h', '-'], input='', text=True)
    values = {line.split()[1]: line.split()[2] for line in defines.splitlines()
              if line.startswith('#define __GLIBC__ ') or line.startswith('#define __GLIBC_MINOR__ ')}
    glibc_version = int(values['__GLIBC__']) * 100 + int(values['__GLIBC_MINOR__'])
    environment.update(LSOF_CC=str(cc), LSOF_CCV='GNU', LSOF_ARCH=target.split('-')[0],
                       LSOF_INCLUDE=str(include), LINUX_INCL=str(include),
                       LINUX_CLIB='-DGLIBCV=' + str(glibc_version), LINUX_HASSELINUX='N',
                       LSOF_VERS='66000', LINUX_VERSION_CODE=str((6 << 16) | (6 << 8)),
                       LSOF_CFGF='-D_GNU_SOURCE', LSOF_CFGL='-ldl',
                       LSOF_AR=ar + ' cr', LSOF_RANLIB=ranlib,
                       LSOF_HOST='none', LSOF_LOGNAME='none',
                       LSOF_BLDCMT='OHOS live socket descriptor backend (opt-in)')
    for label, command in [('configure', ['sh', './Configure', '-n', 'linux']),
                           ('build', ['make', '-j' + str(args.jobs)])]:
        with (output / (label + '.log')).open('w') as log:
            subprocess.run(command, cwd=source, env=environment, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    binary = output / 'steamohos_lsof'
    shutil.copyfile(source / 'lsof', binary)
    binary.chmod(0o755)
    manifest = {'sourceUrl': 'https://deb.debian.org/debian/pool/main/l/lsof/lsof_4.95.0.orig.tar.bz2',
                'sourceSha256': SOURCE_SHA256, 'version': '4.95.0', 'adapted': True,
                'target': target, 'executionModel': 'native_host_test' if args.native_host_test else 'native_aarch64',
                'buildInputs': inputs,
                'patches': {str(p.relative_to(ROOT)): sha(p) for p in [PATCH,REGISTRY_PATCH]},
                'sources': {str(HEADER.relative_to(ROOT)): sha(HEADER)},
                'files': {'steamohos_lsof': {'sha256': sha(binary), 'bytes': binary.stat().st_size}}}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'target': target, 'adapted': True, 'output': str(binary)}))


if __name__ == '__main__':
    main()
