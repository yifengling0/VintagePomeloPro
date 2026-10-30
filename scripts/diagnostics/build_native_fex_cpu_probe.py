#!/usr/bin/env python3
"""Build/reuse the bounded GNU x86 POP/SIMD contract probe; never deploy it."""
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
    parser.add_argument('--bits', choices=['32', '64'], default='64')
    parser.add_argument('--host-check', action='store_true')
    args = parser.parse_args()
    source = Path(__file__).with_name('native_fex_cpu_contract.c')
    compiler = Path(shutil.which('gcc') or '/missing-gcc').resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        parser.error('Requires GNU x86_64 GCC (and multilib when --bits=32)')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / ('native_fex_cpu_' + args.bits)
    manifest = output / ('native-fex-cpu-' + args.bits + '.json')
    options = ['-m' + args.bits, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-msse2']
    inputs = dict(sourceSha256=sha(source), compilerSha256=sha(compiler), options=options)
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('inputs') == inputs and binary.is_file() and old.get('sha256') == sha(binary):
        record = old
    else:
        subprocess.run([str(compiler), *options, str(source), '-o', str(binary)], check=True, timeout=45)
        record = dict(inputs=inputs, sha256=sha(binary), bytes=binary.stat().st_size,
                      architecture='x86_' + args.bits, deviceExecuted=False)
    if args.host_check:
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=35)
        events = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith('HSL_FEX_CPU')]
        record['hostCheck'] = dict(exitCode=result.returncode, events=events,
            passed=result.returncode == 0 and
                'HSL_FEX_CPU_RESULT bits=' + args.bits + ' failures=0' in events)
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    if args.host_check and not record['hostCheck']['passed']:
        raise SystemExit('CPU probe host reference failed')


if __name__ == '__main__':
    main()
