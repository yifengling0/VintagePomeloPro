#!/usr/bin/env python3
"""Test the exact managed header without requiring a patched Wine checkout."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
lock = json.loads((ROOT / 'docs/graphics/graphics-stack.lock.yaml').read_text())
patch_name = 'patches/wine/0080-wineohos-shared-multichannel-downmix.patch'
patch = (ROOT / patch_name).read_bytes()
pin = next(p for p in lock['managed_patches']['wine-valve'] if p['path'] == patch_name)
assert hashlib.sha256(patch).hexdigest() == pin['sha256']
header_name = 'dlls/wineohos.drv/ohos_audio_mix.h'
match = re.search(r'--- /dev/null\n\+\+\+ b/' + re.escape(header_name) +
                  r'\n@@ -0,0 \+1,(\d+) @@\n', patch.decode())
assert match, 'managed patch must create the independent downmix header'
lines = patch.decode()[match.end():].splitlines(keepends=True)
assert len(lines) == int(match[1]) and all(line.startswith('+') for line in lines)
header = ''.join(line[1:] for line in lines).encode()
assert hashlib.sha256(header).hexdigest() == lock['effective_files']['wine-valve'][header_name]
with tempfile.TemporaryDirectory(prefix='vpp-audio-downmix-') as directory:
    directory = Path(directory)
    (directory / 'ohos_audio_mix.h').write_bytes(header)
    output = directory / 'downmix'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11',
                    '-Wall', '-Wextra', '-Werror', '-I', str(directory),
                    str(ROOT / 'host_tests/wine_audio_downmix_test.c'),
                    '-o', str(output), '-lm'], check=True, timeout=30)
    subprocess.run([str(output)], check=True, timeout=10)
