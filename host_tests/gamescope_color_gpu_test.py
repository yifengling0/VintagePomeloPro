#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the built Gamescope shader on host lavapipe, not a math imitation.

This is a correctness test, never a device or performance qualification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from gamescope_source import prepare_recipe


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-headers', type=Path, required=True)
    args = parser.parse_args()
    work = ROOT/'build/gamescope/arm64-v8a'
    # Verify that the linked receipt still represents the current recipe and
    # overlays, rather than silently testing shaders from an earlier revision.
    prepare_recipe(ROOT/'build/gamescope/cache', work)
    out = ROOT/'build/host-tests'
    out.mkdir(parents=True, exist_ok=True)
    report = out/'gamescope-color-gpu.json'
    report.unlink(missing_ok=True)
    receipt = json.loads((work/'native-build.json').read_text())
    if receipt['sourceReceiptSha256'] != digest(work/'source-receipt.json'):
        raise ValueError('Rebuild the core for the current reviewed source first')
    # Keep these specialization values tied to the actual external shader ABI.
    descriptor = (work/'source/src/shaders/descriptor_set.h').read_text()
    for name, value in [('colorspace_sRGB',1), ('colorspace_passthru',4), ('EOTF_Gamma22',0), ('EOTF_Count',2)]:
        if not re.search(r'const int '+name+r'\s*=\s*'+str(value)+r';', descriptor):
            raise ValueError('Shader specialization ABI changed: '+name)
    shader = work/'native-build/src/libgamescope_core.so.p/cs_composite_blit.h'
    binary = out/'gamescope_color_gpu_test'
    subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(args.vulkan_headers),'-I'+str(shader.parent),'-I'+str(work/'source/src/shaders'),
                    str(ROOT/'host_tests/gamescope_color_gpu_test.cpp'),'-ldl','-o',str(binary)],
                   check=True, timeout=60)
    icd = Path('/usr/share/vulkan/icd.d/lvp_icd.json')
    if not icd.is_file():
        raise ValueError('Host lavapipe is required; no implicit GPU/device substitute')
    env = dict(os.environ, VK_ICD_FILENAMES=str(icd), VK_DRIVER_FILES=str(icd))
    results = []
    for name, argv, expected in [('sdr-midtones',[],True), ('sdr-gray',['--gray'],True),
                                 ('raw-normalization-color',['--raw'],True),
                                 ('missing-encode',['--missing-encode'],False),
                                 ('double-encode',['--double-encode'],False),
                                 ('opaque-window-bounds',['--window-bounds'],True),
                                 ('legacy-window-erases-desktop',['--legacy-window-bounds'],False),
                                 ('easu-fp32-odd-extent',['--easu-fp32'],True),
                                 ('easu-fp16-odd-extent',['--easu-fp16'],True),
                                 ('rcas-quad-edge',['--rcas-quad'],True), ('rcas-1px-edge',['--rcas-1px'],True)]:
        result = subprocess.run([str(binary), *argv], env=env, capture_output=True, text=True, timeout=40)
        passed = result.returncode == 0
        if passed != expected or (not expected and 'actual compositor midtone' not in result.stderr):
            raise RuntimeError(name+' failed: '+(result.stdout+result.stderr)[-6000:])
        pixels = [line for line in result.stdout.splitlines() if 'COMPOSITE_PIXEL' in line]
        results.append(dict(name=name, expectedPass=expected, exitCode=result.returncode, pixels=pixels))
        print(name+': '+('PASS' if expected else 'expected regression rejected'))
    report.write_text(json.dumps(dict(
        scope='host lavapipe actual SPIR-V correctness only; no OHOS/device/FPS claim',
        harnessSha256={p.name:digest(p) for p in [Path(__file__), ROOT/'host_tests/gamescope_color_gpu_test.cpp']},
        shaderSha256={p.name:digest(p) for p in [shader, *(shader.parent/(n+'.h') for n in
            ['cs_easu','cs_easu_fp16','cs_composite_rcas','cs_composite_rcas_1px'])]},
        sourceReceiptSha256=receipt['sourceReceiptSha256'],
        results=results),indent=2)+'\n')


if __name__ == '__main__':
    main()
