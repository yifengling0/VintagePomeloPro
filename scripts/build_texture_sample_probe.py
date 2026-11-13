#!/usr/bin/env python3
"""Build the standalone Wine PE Vulkan sampling test without changing runtimes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True, help='x86_64-w64-mingw32-gcc')
    parser.add_argument('--glslang', required=True, help='glslangValidator')
    parser.add_argument('--spirv-val', required=True)
    parser.add_argument('--vulkan-include', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'tests' / 'runtime'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    spv = output / 'vulkan_texture_sample.spv'
    subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.1',
                    str(source / 'vulkan_texture_sample.comp'), '-o', str(spv)],
                   check=True, timeout=60)
    subprocess.run([args.spirv_val, '--target-env', 'vulkan1.1', str(spv)],
                   check=True, timeout=30)
    data = spv.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    header = output / 'vulkan_texture_sample_spv.h'
    header.write_text('/* Generated from vulkan_texture_sample.comp. */\n'
                      'static const uint32_t texture_sample_spv[] = {\n' +
                      ',\n'.join('  ' + ', '.join(f'0x{w:08x}' for w in words[i:i + 8])
                                 for i in range(0, len(words), 8)) + '\n};\n')
    exe = output / 'vp_texture_sample.exe'
    subprocess.run([args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-I', str(args.vulkan_include),
                    '-I', str(output), str(source / 'vulkan_texture_sample_probe.c'),
                    '-o', str(exe), '-lm'], check=True, timeout=120)
    inputs = [source / 'vulkan_texture_sample.comp', source / 'vulkan_texture_sample_probe.c',
              Path(__file__).resolve()]
    manifest = {'schemaVersion': 1, 'scope': 'offscreen-fixture-only; no-runtime-change',
                'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in [*inputs, spv, exe]}}
    (output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'executable': str(exe), 'sha256': manifest['sha256'][exe.name]}))


if __name__ == '__main__':
    main()
