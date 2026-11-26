#!/usr/bin/env python3
"""Build the app with the branch-selected Wine/FEX; reject legacy staging.

VPP_UPSTREAM_RUNTIME_HAP may point to the already audited component package.
Its version/signature do not select the runtime: the committed source locks
and exact component hashes do. Subsequent builds reuse validated staging.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil
from zipfile import ZipFile

from upstream_release_runtime import ROOT, load_lock, stage, verify_hap, verify_staged, file_sha


def run(*command):
    subprocess.run(list(command), cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('hap', 'hap-unsigned', 'app', 'app-unsigned'))
    parser.add_argument('--product', choices=('default', 'proRelease'), default='default')
    args = parser.parse_args()
    lock = load_lock()
    supplied = os.environ.get('VPP_UPSTREAM_RUNTIME_HAP')
    if supplied:
        stage(Path(supplied), lock)
    try:
        verify_staged(lock)
    except (ValueError, FileNotFoundError) as error:
        raise RuntimeError('This branch requires the selected upstream Wine/FEX. '
                           'Stage the verified full component build with '
                           'VPP_UPSTREAM_RUNTIME_HAP; refusing to package the old runtime.') from error
    script = ROOT / 'scripts'
    from local_source_receipt import capture, verify_effective
    from verify_ci_release import application_build_record
    source_path = Path(os.environ.get('SOURCE_BUILD_MANIFEST', ROOT / 'build/selected-app-source.json'))
    if 'SOURCE_BUILD_MANIFEST' not in os.environ:
        source_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.write_text(json.dumps(capture(ROOT), indent=2) + '\n')
    source = json.loads(source_path.read_text())
    if source['commit'] != subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip():
        raise ValueError('Application source receipt belongs to another commit')
    run('git', 'diff', '--quiet', 'HEAD', '--ignore-submodules=all')
    if source.get('sourceMode') == 'local-effective-source':
        verify_effective(ROOT, source)
    app_record = ROOT / 'entry/src/main/resources/rawfile/application-build.json'
    app_record.write_text(json.dumps(application_build_record(source), indent=2) + '\n')
    run('python3', str(script / 'v2_product_contract.py'), '--source-layout')
    run('python3', str(script / 'prepare_windows_steam_runtime.py'), '--verify-only')
    target = 'assembleApp' if args.mode.startswith('app') else 'assembleHap'
    run('hvigorw', '--mode', 'project' if target == 'assembleApp' else 'module',
        '-p', 'product=' + args.product, '-p', 'buildMode=release', target, '--no-daemon')
    hap = ROOT / 'entry/build' / args.product / 'outputs/default/entry-default-unsigned.hap'
    if args.mode.endswith('-unsigned'):
        run('python3', str(script / 'normalize_unsigned_hap.py'), str(hap))
    verify_hap(hap, lock)
    run('python3', str(script / 'verify_ci_release.py'), 'artifact', '--manifest', str(source_path),
        '--hap', str(hap), '--output', str(ROOT / 'build/selected-app-artifact.json'))
    if source.get('sourceMode') == 'local-effective-source':
        verify_effective(ROOT, source)
    for check in ('runtime_library_guard.py', 'v2_product_contract.py', 'prepare_windows_steam_runtime.py'):
        command = ['python3', str(script / check)]
        run(*command, *(['--hap'] if check != 'runtime_library_guard.py' else []), str(hap))
    app = None
    if target == 'assembleApp':
        apps = list((ROOT / 'build/outputs' / args.product).glob('*-unsigned.app'))
        if len(apps) != 1:
            raise ValueError('Expected one unsigned App container')
        app = apps[0]
        with ZipFile(app) as archive:
            embedded = [n for n in archive.namelist() if n.endswith('.hap')]
            if len(embedded) != 1:
                raise ValueError('Expected one embedded HAP')
            with tempfile.SpooledTemporaryFile(max_size=8 * 1024 * 1024) as temp:
                with archive.open(embedded[0]) as src:
                    shutil.copyfileobj(src, temp, 1024 * 1024)
                temp.seek(0)
                verify_hap(temp, lock)
    if not args.mode.endswith('-unsigned'):
        unsigned = app or hap
        signed = unsigned.with_name(unsigned.name.replace('-unsigned.', '-signed.'))
        run('python3', str(ROOT / 'sign.py'), str(unsigned), str(signed))
    out = ROOT / 'build/upstream-release.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    result = dict(schemaVersion=1, sourceCommit=subprocess.check_output(
        ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
        runtimeLockSha256=file_sha(script.parent / 'docs/upstream-sync/release-runtime.lock.json'),
        components={k: lock[k]['adaptedCommit'] for k in ('wine', 'fex')},
        hap=dict(path=str(hap), bytes=hap.stat().st_size, sha256=file_sha(hap)),
        app=dict(path=str(app), bytes=app.stat().st_size, sha256=file_sha(app)) if app else None)
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
