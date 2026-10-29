#!/usr/bin/env python3
"""Build the bounded x86_64 Vulkan memory probe, without downloading dependencies.

Runs under the Linux build host. The binary is executed separately through the
actual Steam/FEX transport; compiling it is not evidence of GPU correctness.
"""
import argparse, hashlib, json, shutil, subprocess
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--memory-type', type=int, default=0, choices=range(32))
    parser.add_argument('--bytes', type=int, default=65536)
    parser.add_argument('--steam-launch', action='store_true')
    args = parser.parse_args()
    if args.bytes < 4 or args.bytes > 64 * 1024 * 1024 or args.bytes % 4:
        parser.error('--bytes must be word-aligned and between 4 and 67108864')
    compiler = Path(shutil.which('gcc') or '/missing-gcc').resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        raise SystemExit('An x86_64-linux-gnu compiler is required')
    source = Path(__file__).with_name('native_vk_coherence_probe.c')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'native_vk_coherence_probe'
    manifest = out / 'native-vk-coherence-build.json'
    inputs = dict(sourceSha256=sha(source), compilerSha256=sha(compiler),
                  memoryType=args.memory_type, allocationBytes=args.bytes,
                  steamArgumentsIgnored=args.steam_launch,
                  vulkanHeaderSha256=sha(Path('/usr/include/vulkan/vulkan_core.h')))
    if manifest.exists() and binary.exists():
        old = json.loads(manifest.read_text())
        if old.get('inputs') == inputs and old.get('sha256') == sha(binary):
            print('Reused verified Vulkan memory probe')
            return
    command = [str(compiler), '-std=c11', '-O2', '-g0', '-Wall', '-Wextra', '-Werror',
               f'-DPROBE_MEMORY_TYPE={args.memory_type}', f'-DPROBE_BYTES={args.bytes}',
               *(['-DPROBE_STEAM_LAUNCH=1'] if args.steam_launch else []),
               str(source), '-Wl,--wrap=__libc_start_main', '-ldl', '-o', str(binary)]
    subprocess.run(command, check=True, timeout=45)
    record = dict(inputs=inputs, command=command, sha256=sha(binary), bytes=binary.stat().st_size,
                  architecture='x86_64-linux-gnu', deviceExecuted=False, gpuExecuted=False)
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({key: record[key] for key in ['sha256', 'bytes', 'architecture']}))


if __name__ == '__main__':
    main()
