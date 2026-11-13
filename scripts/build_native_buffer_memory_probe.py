#!/usr/bin/env python3
"""Build a bounded standalone OHOS probe; never deploy it or modify HAP inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/diagnostics/native_buffer_memory_probe.c'


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('OHOS_NATIVE_SDK',
        r'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native')))
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='Reuse the existing task directory; output is approximately 32 KiB.')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    compiler = sdk / 'llvm/bin' / ('clang.exe' if os.name == 'nt' else 'clang')
    headers = sdk / 'sysroot/usr/include'
    # Feed a single captured source revision to clang. Editors or parallel
    # tasks may rewrite a shared checkout while the compiler is starting.
    source_data = SOURCE.read_bytes()
    inputs = {str(p): sha(p) for p in [SOURCE, SOURCE.with_name('native_image_memory_probe.h'), Path(__file__), compiler,
        headers / 'native_buffer/buffer_common.h', headers / 'native_buffer/native_buffer.h',
        headers / 'native_window/external_window.h', headers / 'native_window/buffer_handle.h',
        headers / 'vulkan/vulkan.h', headers / 'vulkan/vulkan_ohos.h', headers / 'vulkan/vulkan_core.h']}
    inputs[str(SOURCE)] = hashlib.sha256(source_data).hexdigest()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'native-buffer-memory-probe'
    record = out / 'native-buffer-memory-probe-build.json'
    command = [str(compiler), '--target=aarch64-linux-ohos', '--sysroot=' + str(sdk / 'sysroot'),
        '-std=c11', '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-I' + str(SOURCE.parent), '-x', 'c', '-',
        '-Wl,-z,relro,-z,now', '-ldl', '-lnative_buffer', '-lnative_window', '-o', str(binary)]
    if record.exists() and binary.is_file():
        prior = json.loads(record.read_text())
        if prior.get('inputs') == inputs and prior.get('command') == command and prior.get('sha256') == sha(binary):
            print('Reused standalone OHOS NativeBuffer memory probe; not run on a device')
            return
    with (out / 'native-buffer-memory-probe-build.log').open('w') as log:
        subprocess.run(command, input=source_data, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    if any(sha(Path(path)) != digest for path, digest in inputs.items()):
        raise RuntimeError('Probe source/toolchain changed during build; no valid build record was written')
    prefix = binary.read_bytes()[:20]
    if prefix[:6] != b'\x7fELF\x02\x01' or int.from_bytes(prefix[18:20], 'little') != 183:
        raise ValueError('Expected AArch64 ELF')
    result = dict(inputs=inputs, command=command, architecture='aarch64-linux-ohos',
        bytes=binary.stat().st_size, sha256=sha(binary), deviceExecuted=False,
        scope='system Vulkan buffer import/export + optimal UNORM/sRGB images; synthetic pixels only; no presented readback, Venus or FEX',
        bounds='two 64 KiB staging buffers; image <=4 MiB; at most six image cases, eight rounds each; 5s per fence, 75s process alarm; outer 90s timeout',
        appPayloadModified=False)
    record.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ['architecture', 'bytes', 'sha256', 'deviceExecuted', 'appPayloadModified']}))


if __name__ == '__main__':
    main()
