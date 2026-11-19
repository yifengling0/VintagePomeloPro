#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the actual OHOS swapchain creation and storage view methods with failure injection."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from source_function import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-headers', type=Path, required=True)
    args = parser.parse_args()
    backend = (ROOT/'native/gamescope/ohos/OhosBackend.cpp').read_text()
    embedded = (ROOT/'native/gamescope/ohos/Embedded.cpp').read_text()
    out = ROOT/'build/host-tests'
    out.mkdir(parents=True, exist_ok=True)
    methods = out/'gamescope_output_methods.h'
    methods.write_text(function(embedded, 'bool CVulkanTexture::BInitOhosStorageOutput(')+'\n'+
        function(backend, 'bool COhosBackend::recreateOutput()')+'\n'+
        function(backend, 'void COhosBackend::RecordShmUpload(')+'\n')
    (out/'gamescope_output_cleanup.h').write_text(function(backend, 'void destroyOutput(Output& output)')+'\n')
    binary = out/'gamescope_output_test'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-Wno-missing-field-initializers', '-I'+str(args.vulkan_headers), '-I'+str(out),
        str(ROOT/'host_tests/gamescope_output_test.cpp'), '-o', str(binary)], check=True, timeout=60)
    result = subprocess.run([binary], text=True, capture_output=True, timeout=20)
    if result.returncode:
        raise RuntimeError((result.stdout+result.stderr)[-6000:])
    print(result.stdout.strip())
    (out/'gamescope-output.json').write_text(json.dumps(dict(
        scope='host actual output creation/storage-view methods and output/SHM command recording; no WSI/device claim',
        methodsSha256=hashlib.sha256(methods.read_bytes()).hexdigest(),
        result=result.stdout.strip()), indent=2)+'\n')


if __name__ == '__main__':
    main()
