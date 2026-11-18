#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Package a Gamescope HAP using an existing payload.

Run only through Makefile in winehua-dev. Never updates formal release files.
Signing configuration and verification outputs remain private and temporary.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import subprocess
import tempfile
import time
import zipfile
from local_source_receipt import capture, verify_effective
from verify_ci_release import application_build_record
from gamescope_source import prepare_recipe
from configure_value_added import load_json5

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def quiet(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, timeout=240)
    if result.returncode:
        raise RuntimeError('Signing or signature verification failed; private tool output suppressed')
    return result.stdout.decode(errors='replace')


def package(sdk, config, material, output, unsigned_only=False):
    work = ROOT/'build/gamescope/arm64-v8a'
    payload = ROOT/'entry/libs/arm64-v8a'
    if unsigned_only:
        product = next(p for p in load_json5(ROOT/'build-profile.json5')['app']['products']
                       if p['name'] == 'proRelease')
        if product.get('signingConfig'):
            raise ValueError('Unsigned packaging requires an unsigned proRelease product')
    # Freeze committed app code and the actual local vendor overlays. The
    # existing Wine/FEX payload keeps its independent component provenance.
    source_manifest = capture(ROOT)
    record = application_build_record(source_manifest)
    (work/'app-source.json').write_text(json.dumps(source_manifest, indent=2)+'\n')
    (ROOT/'entry/src/main/resources/rawfile/application-build.json').write_text(
        json.dumps(record, indent=2)+'\n')
    prepare_recipe(ROOT/'build/gamescope/cache', work)
    # A bounded, separate X11 pixel source for device acceptance. It is not a
    # production target and is removed from entry/libs after this debug build.
    probe = work/'libgamescope_pixel_probe.so'
    probe_source = ROOT/'host_tests/gamescope_x11_probe.cpp'
    if not unsigned_only and (not probe.exists() or probe.stat().st_mtime < probe_source.stat().st_mtime):
        subprocess.run([work/'deps/tools/cxx', '-std=c++20', '-O2', '-fPIC', '-shared',
                        '-I'+str(work/'deps/host-view/include'), probe_source,
                        '-L'+str(work/'deps/host-view/lib'), '-lX11', '-ldl', '-o', probe], check=True)
    receipt = json.loads((work/'native-build.json').read_text())
    if receipt.get('sourceReceiptSha256') != sha(work/'source-receipt.json'):
        raise ValueError('Core build belongs to another source recipe')
    library = Path(receipt['library'])
    if library.stat().st_size != receipt['bytes'] or sha(library) != receipt['sha256']:
        raise ValueError('Core build identity mismatch')
    # Only the core and four newly compiled X libraries are added. Every other
    # Wine/FEX/Mesa/backend library is the existing validated payload.
    added = {library.name: library}
    for name in ('libXdamage.so.1', 'libXtst.so.6', 'libXRes.so.1', 'libXmuu.so.1'):
        added[name] = work/'deps/usr/lib'/name
    for name, source in added.items():
        if not (payload/name).is_file() or sha(payload/name) != sha(source):
            shutil.copyfile(source, payload/name)
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, VPP_HOST_EXT_ROOT='/shared/build/host-ext')
    log = work/('unsigned-package.log' if unsigned_only else 'debug-package.log')
    probe_payload = payload/probe.name
    if unsigned_only and probe_payload.exists():
        raise ValueError('Remove the development pixel probe from production staging before packaging')
    try:
        if not unsigned_only:
            shutil.copyfile(probe, probe_payload)
        with log.open('w') as stream:
            subprocess.run([sdk/'bin/hvigorw', '--mode', 'module', '-p', 'product=proRelease',
                            '-p', 'buildMode=release', 'assembleHap'], cwd=ROOT, env=environment,
                           stdout=stream, stderr=subprocess.STDOUT, check=True)
    finally:
        if not unsigned_only:
            probe_payload.unlink(missing_ok=True)
    # The build log is bounded by this single increment, never a device trace.
    if log.stat().st_size > 32*1024**2:
        raise ValueError('Unexpectedly large package log')
    unsigned = ROOT/'entry/build/proRelease/outputs/default/entry-default-unsigned.hap'
    with zipfile.ZipFile(unsigned) as archive:
        if archive.testzip() is not None:
            raise ValueError('Corrupt HAP')
        module = json.loads(archive.read('module.json'))
        if module['app']['bundleName'] != 'com.vintage.pomelopro':
            raise ValueError('Unexpected application identity')
        if any(module['app'][key] != value for key, value in record['app'].items()):
            raise ValueError('Packaged application version differs from the source receipt')
        if 'ohos.permission.READ_PASTEBOARD' not in {p['name'] for p in module['module'].get('requestPermissions', [])}:
            raise ValueError('Missing clipboard permission declaration')
        if json.loads(archive.read('resources/rawfile/application-build.json')) != record:
            raise ValueError('Packaged application source identity mismatch')
        for name, source in (
            ('WINFG-LICENSE.txt', 'native/gamescope/winfg/upstream/LICENSE'),
            ('WINFG-FIDELITYFX-NOTICE.md', 'native/gamescope/winfg/upstream/NOTICE_FIDELITYFX_OPTICALFLOW.md'),
        ):
            if archive.read('resources/rawfile/'+name).replace(b'\r\n', b'\n') != (ROOT/source).read_bytes().replace(b'\r\n', b'\n'):
                raise ValueError('Missing or incorrect Win-FG license: '+name)
        if 'libs/arm64-v8a/libgamescope_child.so' not in archive.namelist():
            raise ValueError('Missing Gamescope process entry')
        # Verify every staged core library after Hvigor's stripping pass by
        # comparing the loadable ELF segments in the package audit separately.
        for name in added:
            if 'libs/arm64-v8a/'+name not in archive.namelist():
                raise ValueError('Missing Gamescope dependency: '+name)
        if unsigned_only:
            if module['app'].get('debug', False):
                raise ValueError('Unsigned distribution HAP must not be debuggable')
            if any(PureWindowsPath(name).name == probe.name for name in archive.namelist()):
                raise ValueError('Unsigned distribution HAP contains the development pixel probe')
            if 'phone' not in module['module'].get('deviceTypes', []):
                raise ValueError('Missing phone device declaration')
            if {name.split('/')[1] for name in archive.namelist() if name.startswith('libs/')} != {'arm64-v8a'}:
                raise ValueError('Unexpected native package architecture')
    if unsigned_only:
        verify_effective(ROOT, source_manifest)
        if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() != source_manifest['commit']:
            raise ValueError('Application commit changed during packaging')
        subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--ignore-submodules=all'], cwd=ROOT, check=True)
        target = output/('VPP-'+module['app']['versionName']+'-arm64-unsigned.hap')
        if not target.exists() or sha(target) != sha(unsigned):
            shutil.copyfile(unsigned, target)
        result = {'bundle': module['app']['bundleName'], 'version': module['app']['versionName'],
                  'versionCode': module['app']['versionCode'], 'bytes': target.stat().st_size,
                  'sha256': sha(target), 'path': str(target), 'signing': 'unsigned',
                  'debug': False, 'pixelProbe': False, 'renderQualified': False,
                  'head': source_manifest['commit'], 'applicationBuild': record,
                  'coreSourceReceiptSha256': receipt['sourceReceiptSha256'],
                  'coreBuildSha256': receipt['sha256']}
        (output/'package-unsigned.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result))
        return
    jar = sdk/'sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar'
    text = config.read_text()
    settings = json.loads(re.sub(r',\s*([}\]])', r'\1', re.sub(r'//.*$', '', text, flags=re.M)))
    selected = copy.deepcopy(next(c for c in settings['app']['signingConfigs'] if c['name'] == 'default'))
    for key in ('certpath', 'profile', 'storeFile'):
        selected['material'][key] = str(material/PureWindowsPath(selected['material'][key]).name)
    profile = Path(selected['material']['profile'])
    pending = work/'debug-unsigned.hap'
    signed = output/('VPP-'+module['app']['versionName']+'-gamescope-debug-signed.hap')
    try:
        with tempfile.TemporaryDirectory(prefix='vpp-gamescope-sign-', dir='/dev/shm') as directory:
            private = Path(directory)
            quiet(['java', '-jar', jar, 'verify-profile', '-inFile', profile, '-outFile', private/'profile.json'])
            verified = json.loads((private/'profile.json').read_text())
            info = verified['content']
            if (not verified['verifiedPassed'] or info['type'] != 'debug' or
                info['bundle-info']['bundle-name'] != module['app']['bundleName'] or
                not info['validity']['not-before'] <= time.time() <= info['validity']['not-after'] or
                'ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY' not in info['acls']['allowed-acls'] or
                'ohos.permission.READ_PASTEBOARD' not in info['acls']['allowed-acls'] or
                info['bundle-info']['development-certificate'].strip() not in
                    Path(selected['material']['certpath']).read_text()):
                raise ValueError('Debug signing identity/profile mismatch')
            with zipfile.ZipFile(unsigned) as source, zipfile.ZipFile(pending, 'w', allowZip64=True) as target:
                for member in source.infolist():
                    if member.filename == '.pages.info' or member.filename.startswith('META-INF/'):
                        continue
                    if member.filename == 'module.json':
                        module['app']['debug'] = True
                        target.writestr(copy.copy(member), json.dumps(module, separators=(',', ':'), ensure_ascii=False).encode())
                    else:
                        with source.open(member) as src, target.open(copy.copy(member), 'w') as dst:
                            shutil.copyfileobj(src, dst, 1024**2)
            private_config = private/'config.json'
            private_config.write_text(json.dumps({'app': {'signingConfigs': [selected]}}))
            private_config.chmod(0o600)
            quiet(['python3', ROOT/'sign.py', pending, signed, private_config, 'default'])
            quiet(['java', '-jar', jar, 'verify-app', '-inFile', signed,
                   '-outProfile', private/'actual.p7b', '-outCertChain', private/'chain.cer'])
            if sha(private/'actual.p7b') != sha(profile):
                raise ValueError('Signed HAP has unexpected profile')
        verify_effective(ROOT, source_manifest)
        if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() != source_manifest['commit']:
            raise ValueError('Application commit changed during packaging')
        subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--ignore-submodules=all'], cwd=ROOT, check=True)
        with zipfile.ZipFile(signed) as archive:
            if json.loads(archive.read('resources/rawfile/application-build.json')) != record:
                raise ValueError('Signed application source identity mismatch')
    finally:
        pending.unlink(missing_ok=True)
    result = {'bundle': module['app']['bundleName'], 'version': module['app']['versionName'],
              'bytes': signed.stat().st_size, 'sha256': sha(signed), 'path': str(signed),
              'debugSignatureVerified': True, 'renderQualified': False,
              'head': source_manifest['commit'], 'applicationBuild': record,
              'coreSourceReceiptSha256': receipt['sourceReceiptSha256'],
              'coreBuildSha256': receipt['sha256']}
    (output/'package.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--unsigned', action='store_true', help='Build a release HAP without signing or test probes')
    parser.add_argument('--signing-config', type=Path)
    parser.add_argument('--signing-material', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.unsigned and (args.signing_config is None or args.signing_material is None):
        parser.error('Debug signing requires --signing-config and --signing-material')
    package(args.sdk, args.signing_config, args.signing_material, args.output, args.unsigned)
