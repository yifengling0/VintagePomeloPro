#!/usr/bin/env python3
"""Build the real Vulkan transfer regression for Wine ARM64EC or x64.

The executable takes: log-path [MiB=96] [submit2=0] [rounds=1]
[map-before-bind=0]. Success requires FINAL success and process exit zero.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arch', choices=('arm64ec', 'x86_64'), default='arm64ec')
parser.add_argument('--llvm-mingw', default=os.environ.get('LLVM_MINGW'))
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
if not args.llvm_mingw:
    parser.error('set LLVM_MINGW or pass --llvm-mingw; no toolchain is downloaded')
tool = Path(args.llvm_mingw) / 'bin'
args.output.mkdir(parents=True, exist_ok=True)
source = root / 'tests/runtime/vulkan_shadow_transfer_probe.c'
core = source.with_name('vulkan_shadow_transfer_core.c')
exports = sorted(set(re.findall(r'\b(vk[A-Z]\w+)\(', source.read_text() + core.read_text())))
definition = args.output / 'vulkan.def'
definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n' + '\n'.join(exports) + '\n')
library = args.output / 'libvulkan-1.a'
subprocess.run([str(tool / 'llvm-dlltool'), '-m',
    'arm64ec' if args.arch == 'arm64ec' else 'i386:x86-64',
    '-d', str(definition), '-l', str(library)], check=True)
output = args.output / ('vulkan_shadow_transfer-' + args.arch + '.exe')
subprocess.run([str(tool / (args.arch + '-w64-mingw32-clang')), str(source),
    '-I' + str(root / 'thirdparty/dxvk-modern/include/vulkan/include'),
    '-O2', str(library), '-o', str(output)], check=True)
print(output)
