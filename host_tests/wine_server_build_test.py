#!/usr/bin/env python3
"""Run the production server build function with a recording compiler.

Check the release flags and cache invalidation; no Wine source or SDK needed.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
script = (repo / 'scripts/build_wine.sh').read_text()
function = 'build_wineserver() {' + script.split('build_wineserver() {', 1)[1].split('\n# ---- main ----', 1)[0]
env_script = (repo / 'scripts/env.sh').read_text()
validation = 'require_optimization_flags() {' + env_script.split('require_optimization_flags() {', 1)[1].split('\nrequire_ndebug()', 1)[0]

with tempfile.TemporaryDirectory(prefix='wine-server-build-') as temporary:
    root = Path(temporary)
    source = root / 'wine-source'
    (source / 'server').mkdir(parents=True)
    for name in ('event.c', 'semaphore.c'):
        (source / 'server' / name).write_text('/* test input */\n')
    compiler = root / 'compiler'
    compiler.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
if args == ['--version']:
    print(os.environ.get('COMPILER_VERSION', 'test-1'))
    raise SystemExit(0)
with open(os.environ['CALLS_FILE'], 'a') as out:
    out.write(json.dumps(args) + '\\n')
pathlib.Path(args[args.index('-o') + 1]).write_text('compiled artifact')
''')
    compiler.chmod(0o755)
    calls_file = root / 'calls.jsonl'
    build = root / 'build'
    libs = root / 'native-libs'
    shell = root / 'build.sh'
    shell.write_text('''#!/bin/bash
set -euo pipefail
log() { :; }
err() { echo "$*" >&2; exit 1; }
''' + validation + function + '\nbuild_wineserver\n')
    env = dict(os.environ, WINE_SRC=str(source), BUILD_DIR=str(build),
               WINE_ARCH='aarch64', NATIVE_ARCH='arm64-v8a', NATIVE_LIBS=str(libs),
               WINE_DEVICE_ROOT='/test/prefix', TARGET='aarch64-linux-ohos',
               SYSROOT='/test/sysroot', CLANG=str(compiler),
               WINE_CFLAGS='-g -O2 -fPIC -D__OHOS__', CALLS_FILE=str(calls_file))

    def run(**changes):
        return subprocess.run(['bash', str(shell)], env={**env, **changes},
                              capture_output=True, text=True)

    def calls():
        return [json.loads(line) for line in calls_file.read_text().splitlines()] if calls_file.exists() else []

    # An old binary with newer mtimes and no flags record must be rebuilt.
    server = build / 'wine_server-aarch64' / 'libwineserver.so'
    server.parent.mkdir(parents=True)
    server.write_text('old O0 artifact')
    assert run().returncode == 0
    assert len(calls()) == 3, calls()
    for invocation in calls()[:2]:
        assert '-O2' in invocation and '-g' in invocation
        assert '-fno-strict-aliasing' in invocation
        assert '-DNDEBUG' not in invocation  # Preserve Wine assertions.
    assert (libs / 'libwineserver.so').read_text() == 'compiled artifact'

    assert run().returncode == 0
    assert len(calls()) == 3  # Identical compiler/flags can reuse the output.
    (libs / 'libwineserver.so').write_text('library from a different build')
    linked_backup = root / 'old-library.so'
    linked_backup.hardlink_to(libs / 'libwineserver.so')
    assert run().returncode == 0
    assert len(calls()) == 3
    assert (libs / 'libwineserver.so').read_text() == 'compiled artifact'
    assert linked_backup.read_text() == 'library from a different build'
    (libs / 'libwineserver.so').unlink()
    assert run().returncode == 0
    assert len(calls()) == 3 and (libs / 'libwineserver.so').is_file()

    assert run(COMPILER_VERSION='test-2').returncode == 0
    assert len(calls()) == 6  # Compiler change recompiles every server object.
    assert run(WINE_CFLAGS='-g -O3 -fPIC -D__OHOS__').returncode == 0
    assert len(calls()) == 9  # Flags changes cannot keep old O0 objects.
    rejected = run(WINE_CFLAGS='-g -fPIC -D__OHOS__')
    assert rejected.returncode and 'no optimization flag' in rejected.stderr
    assert len(calls()) == 9

print('PASS: release/aliasing flags, assertions, old cache, compiler/flags invalidation, cache reuse')
