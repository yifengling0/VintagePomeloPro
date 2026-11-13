#!/usr/bin/env python3
"""Build the real D3D11 BC sampler/transfer regression without downloading tools."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-dir', type=Path, required=True)
    parser.add_argument('--bcdec-include', type=Path, required=True,
                        help='Wine dlls/d3dx9_36 containing the MIT bcdec.h')
    parser.add_argument('--arch', choices=('x86_64', 'i686', 'arm64ec'), default='x86_64')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'tests/runtime/d3d11_virtual_bc_probe.cpp'
    decoder = args.bcdec_include.resolve() / 'bcdec.h'
    if not decoder.is_file():
        parser.error('--bcdec-include must contain bcdec.h')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = output / 'vp_virtual_bc.exe'
    compiler = args.compiler_dir.resolve() / (args.arch + '-w64-mingw32-clang++')
    subprocess.run([str(compiler), '-std=c++17', '-O2', '-static',
                    '-I' + str(decoder.parent), str(source), '-ld3d11', '-ldxgi',
                    '-ld3dcompiler', '-o', str(exe)], check=True, timeout=120)
    record = {'architecture': args.arch,
              'files': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in (source, decoder, exe)}}
    (output / 'probe-build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(exe)


if __name__ == '__main__':
    main()
