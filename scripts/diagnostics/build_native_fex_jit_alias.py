#!/usr/bin/env python3
"""Build/reuse a small GNU x86-64 JIT mapping probe; never deploy it."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--host-check', action='store_true')
    args = parser.parse_args()
    source = Path(__file__).with_name('native_fex_jit_alias.c')
    compiler = Path(shutil.which('gcc') or '/missing-gcc').resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        parser.error('Requires GNU x86_64 GCC')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary, manifest = output / 'native_fex_jit_alias', output / 'native-fex-jit-alias.json'
    options = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread']
    inputs = dict(sourceSha256=sha(source), compilerSha256=sha(compiler), options=options)
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('inputs') == inputs and binary.is_file() and old.get('sha256') == sha(binary):
        record = old
    else:
        subprocess.run([str(compiler), *options, str(source), '-o', str(binary)], check=True, timeout=45)
        record = dict(inputs=inputs, sha256=sha(binary), bytes=binary.stat().st_size,
                      architecture='x86_64-linux-gnu', deviceExecuted=False)
    if args.host_check:
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=50)
        events = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith('NATIVE_JIT')]
        record['hostCheck'] = dict(exitCode=result.returncode, events=events,
                                  passed=result.returncode == 0 and 'NATIVE_JIT_RESULT failures=0' in events)
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    if args.host_check and not record['hostCheck']['passed']:
        raise SystemExit('JIT probe host reference failed')


if __name__ == '__main__':
    main()
