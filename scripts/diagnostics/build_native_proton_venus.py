#!/usr/bin/env python3
"""Build the experimental native-memory Venus variant from an isolated source tree.

This diagnostic entry point does not change the pinned release build recipe or
replace a packaged driver. Use an existing dedicated native-memory build tree.
"""
import argparse
from pathlib import Path
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
for name in ['source', 'build', 'output', 'meson-tools']:
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
source = args.source.resolve(strict=True)
if source.is_relative_to(ROOT) or args.output.resolve().is_relative_to(source):
    parser.error('Use an isolated Mesa source tree and a separate output directory')
patch = ROOT / 'patches/mesa/0010-venus-ohos-native-memory-context.patch'
command = ['patch', '-d', str(source), '-p1', '--batch', '--fuzz=0']
if subprocess.run(command + ['--dry-run', '-R', '-i', str(patch)],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
    subprocess.run(command + ['--dry-run', '-i', str(patch)], check=True)
    subprocess.run(command + ['--no-backup-if-mismatch', '-i', str(patch)], check=True)
runpy.run_path(str(ROOT / 'scripts/build_linux_proton_venus.py'), run_name='__main__')
