#!/usr/bin/env python3
"""Build the isolated real Linux Steam account-readiness probe."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', type=Path, required=True, help='GNU AArch64 gcc')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
source = repo / 'entry/src/main/cpp/linux_steam/gnu_fixture/proton_steam_ready.cpp'
compiler = args.cc.resolve(strict=True).with_name(args.cc.name.replace('gcc', 'g++'))
assert subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip().startswith('aarch64')
args.output.mkdir(parents=True, exist_ok=True)
binary = args.output / 'proton-steam-ready'
record = args.output / 'proton-steam-ready-identity.json'
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
inputs = {str(path): sha(path) for path in [source, compiler, Path(__file__)]}
old = json.loads(record.read_text()) if record.exists() else {}
if old.get('inputs') == inputs and binary.exists() and old.get('sha256') == sha(binary):
    print('Reused verified GNU Steam readiness probe')
else:
    subprocess.run([str(compiler), '-O2', '-Wall', '-Wextra', '-Werror', str(source),
                    '-ldl', '-pthread', '-o', str(binary)], check=True)
    record.write_text(json.dumps({'inputs': inputs, 'bytes': binary.stat().st_size,
                                  'sha256': sha(binary)}, indent=2) + '\n')
    print('Built GNU Steam readiness probe')
