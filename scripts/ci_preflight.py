#!/usr/bin/env python3
"""Collect build-input and contract failures before the expensive native build."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SUITES = (
    'host_tests/ci_preflight_test.py', 'host_tests/ci_release_test.py', 'host_tests/linux_runtime_components_test.py',
    'host_tests/source_build_contract_test.py', 'host_tests/linux_steam_pe_components_test.py',
    'host_tests/smoke_build_arch_test.py', 'host_tests/wine_build_identity_test.py',
    'host_tests/gstreamer_native_plugins_test.py', 'host_tests/runtime_library_guard_test.py',
    'host_tests/proton_runtime_catalog_test.py', 'scripts/test_check_runtime_components.py', 'host_tests/graphics_assembly_manifest_test.py',
)


def package_inputs(root, tracked):
    errors = []
    for relative in ('oh-package.json5', 'entry/oh-package.json5'):
        package = root / relative
        declaration = json.loads(package.read_text())
        lock = json.loads(package.with_name('oh-package-lock.json5').read_text())
        dependencies = dict(declaration.get('dependencies', {}), **declaration.get('devDependencies', {}))
        for name, value in dependencies.items():
            local = value.startswith('file:')
            version = value[5:].removeprefix('./') if local else value
            key = name + '@' + version
            row = lock['packages'].get(key, {})
            if lock['specifiers'].get(key) != key or row.get('name') != name:
                errors.append(relative + ': missing lock entry ' + key)
            if local:
                directory = package.parent / version
                metadata = json.loads((directory / 'oh-package.json5').read_text())
                if row.get('version') != metadata['version'] or row.get('resolved') != version:
                    errors.append(relative + ': stale local dependency ' + name)
                for path in directory.rglob('*'):
                    if path.is_file() and path.relative_to(root).as_posix() not in tracked:
                        errors.append('Uncommitted native type input: ' + str(path.relative_to(root)))
    lock = json.loads((root / 'docs/linux-steam/build-components.lock.json').read_text())
    for relative, expected in lock['sourceInputs'].items():
        path = root / relative
        if relative not in tracked or not path.is_file():
            errors.append('Uncommitted GNU dependency input: ' + relative)
        elif hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            errors.append('GNU dependency input changed without a new dependency lock: ' + relative)
    if errors:
        raise ValueError('\n'.join(errors))


def runtime_inputs(root, tracked):
    required = (
        'entry/src/main/ets/model/LinuxProtonRuntimeModels.ets',
        'entry/src/main/ets/service/LinuxProtonRuntimeService.ets',
        'entry/src/main/ets/components/hmosdeck/ProtonRuntimePanel.ets',
        'entry/src/main/resources/rawfile/linux-proton/runtime-catalog.json',
        'docs/linux-steam/proton-runtimes/catalog.json', 'scripts/build_proton_runtime_catalog.py',
        'host_tests/proton_runtime_catalog_test.py', 'scripts/run_linux_proton_component_tests.cjs',
    )
    missing = [name for name in required if name not in tracked or not (root / name).is_file()]
    if missing: raise ValueError('Uncommitted Proton manager input: ' + ', '.join(missing))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--sdk', type=Path, help='OHOS SDK root, for full Native CMake source configuration')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/ci-preflight-cmake')
    parser.add_argument('--skip-tests', action='store_true', help='Tests already ran in the first CI job')
    args = parser.parse_args()
    rows = []
    def check(name, action):
        try:
            action()
            row = dict(check=name, result='PASS')
        except (OSError, ValueError, SyntaxError, subprocess.SubprocessError) as error:
            row = dict(check=name, result='FAIL', detail=str(error)[-6000:])
        rows.append(row)
        print(row['result'] + ': ' + name, flush=True)
        if 'detail' in row:
            print(row['detail'], flush=True)
    def command(argv):
        result = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=180)
        if result.returncode:
            raise ValueError(' '.join(map(str, argv)) + '\n' + result.stdout[-5000:])
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0'))
    check('committed Proton manager inputs', lambda: runtime_inputs(ROOT, tracked))
    check('independent Proton runtime catalog', lambda: command([sys.executable, '-B', 'scripts/build_proton_runtime_catalog.py']))
    check('declared dependency locks and committed GNU inputs', lambda: package_inputs(ROOT, tracked))
    for path in sorted((ROOT / 'scripts').glob('*.sh')):
        check(str(path.relative_to(ROOT)), lambda p=path: command(['bash', '-n', str(p)]))
    for path in sorted((ROOT / 'scripts').glob('*.py')):
        check(str(path.relative_to(ROOT)), lambda p=path: ast.parse(p.read_text(), filename=str(p)))
    if not args.skip_tests:
        for suite in SUITES:
            check(suite, lambda p=suite: command([sys.executable, '-B', p]))
    if args.sdk:
        def configure():
            query = args.build_dir / '.cmake/api/v1/query'
            query.mkdir(parents=True, exist_ok=True)
            (query / 'codemodel-v2').touch()
            command(['cmake', '-S', str(ROOT / 'entry/src/main/cpp'), '-B', str(args.build_dir),
                '-DCMAKE_TOOLCHAIN_FILE=' + str(args.sdk / 'native/build/cmake/ohos.toolchain.cmake'),
                '-DOHOS_ARCH=arm64-v8a', '-DOHOS_PLATFORM=OHOS', '-DCMAKE_BUILD_TYPE=Release',
                '-DWINEHUA_ENABLE_STEAMOHOS_HOST=ON', '-DWINEHUA_ENABLE_STEAMOHOS_GNU_EXEC=ON'])
            from source_build_contract import verify_native_targets
            reply = args.build_dir / '.cmake/api/v1/reply'
            index = json.loads(max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime_ns).read_text())
            model = json.loads((reply / index['reply']['codemodel-v2']['jsonFile']).read_text())
            missing = set()
            modules = set()
            for reference in model['configurations'][0]['targets']:
                target = json.loads((reply / reference['jsonFile']).read_text())
                if target['type'] == 'SHARED_LIBRARY':
                    modules.update('libs/arm64-v8a/' + Path(a['path']).name for a in target['artifacts'])
                for source in target.get('sources', []):
                    if source.get('isGenerated'):
                        continue
                    path = (ROOT / 'entry/src/main/cpp' / source['path']).resolve()
                    if not path.is_relative_to(ROOT):
                        raise ValueError('Native source outside checkout: ' + str(path))
                    relative = path.relative_to(ROOT).as_posix()
                    if not relative.startswith('build/native_') and relative not in tracked:
                        missing.add(relative)
            verify_native_targets(modules)
            if missing:
                raise ValueError('CMake depends on uncommitted native sources: ' + ', '.join(sorted(missing)))
        check('full Native CMake configuration and committed target sources', configure)
    report = dict(commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  result='FAIL' if any(r['result'] == 'FAIL' for r in rows) else 'PASS', checks=rows)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(str(len(rows)) + ' checks; ' + report['result'])
    return 1 if report['result'] == 'FAIL' else 0


if __name__ == '__main__':
    raise SystemExit(main())
