#!/usr/bin/env python3
"""Build a pinned, standalone BC-to-ASTC benchmark. Does not download or deploy."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ASTC_REVISION = '2319d9c4d4af53a7fc7c52985e264ce6e8a02a9b'  # Arm 5.2.0, Apache-2.0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-dir', type=Path, required=True, help='llvm-mingw bin directory')
    parser.add_argument('--astc-source', type=Path, required=True, help='clean astc-encoder 5.2.0 Git checkout')
    parser.add_argument('--bcdec-include', type=Path, required=True, help='Wine dlls/d3dx9_36 directory (MIT decoder)')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    astc_source = args.astc_source.resolve()
    revision = subprocess.check_output(['git', '-C', str(astc_source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(astc_source), 'status', '--porcelain'], text=True).strip()
    if revision != ASTC_REVISION or dirty:
        raise SystemExit('Requires the clean, pinned Arm astc-encoder 5.2.0 checkout')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build = output / 'astc-build'
    source = Path(__file__).resolve().parents[1] / 'tests/runtime/bc_astc_benchmark.cpp'
    compiler = args.compiler_dir.resolve()
    subprocess.run(['cmake', '-S', str(astc_source), '-B', str(build),
                    '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=AMD64',
                    f'-DCMAKE_C_COMPILER={compiler / "x86_64-w64-mingw32-gcc"}',
                    f'-DCMAKE_CXX_COMPILER={compiler / "x86_64-w64-mingw32-g++"}',
                    '-DCMAKE_BUILD_TYPE=Release', '-DASTCENC_CLI=OFF',
                    '-DASTCENC_ISA_SSE2=ON', '-DASTCENC_ISA_NATIVE=OFF',
                    '-DASTCENC_BLOCK_MAX_TEXELS=16'], check=True, timeout=120)
    subprocess.run(['cmake', '--build', str(build), '-j', '8'], check=True, timeout=180)
    exe = output / 'vp_bc_astc_benchmark.exe'
    subprocess.run([str(compiler / 'x86_64-w64-mingw32-g++'), '-std=c++17', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-static',
                    '-I', str(astc_source / 'Source'), '-I', str(args.bcdec_include),
                    str(source), str(build / 'Source/libastcenc-sse2-static.a'),
                    '-o', str(exe)], check=True, timeout=120)
    files = [source, Path(__file__).resolve(), args.bcdec_include / 'bcdec.h',
             astc_source / 'Source/astcenc.h', exe]
    manifest = {'schemaVersion': 1, 'scope': 'standalone-benchmark; no-runtime-change',
                'astcRevision': revision, 'isa': 'sse2', 'codecThreads': 1,
                'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (output / 'bc-astc-build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'executable': str(exe), 'sha256': manifest['sha256'][exe.name]}))


if __name__ == '__main__':
    main()
