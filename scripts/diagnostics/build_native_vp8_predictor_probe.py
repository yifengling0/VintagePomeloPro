#!/usr/bin/env python3
"""Build/reuse a bounded predictor probe against an explicitly identified ELF.

No library or game asset is modified. Offsets are diagnostic inputs, never
automatic per-game patches. Run the resulting executable through the selected
device's Steam/FEX path separately; host success is not device success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def elf_segment(data, address, size, flags):
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 62:
        raise ValueError('Requires an x86_64 little-endian ELF')
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride < 56 or offset + stride * count > len(data):
        raise ValueError('Invalid ELF program header bounds')
    for index in range(count):
        kind, permissions, file_offset, va, _, file_size, _, _ = struct.unpack_from(
            '<IIQQQQQQ', data, offset + index * stride)
        if (kind == 1 and permissions & flags == flags and va <= address and
                size <= file_size and address - va <= file_size - size and
                file_offset + file_size <= len(data)):
            start = file_offset + address - va
            return data[start:start + size]
    raise ValueError('Offset is outside the required file-backed ELF segment')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--library-sha256', required=True)
    parser.add_argument('--runtime-library', required=True)
    parser.add_argument('--function-offset', type=lambda v: int(v, 0), required=True)
    parser.add_argument('--version-offset', type=lambda v: int(v, 0), required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--host-check', action='store_true')
    parser.add_argument('--signal-stress', action='store_true',
                        help='Deliver targeted SIGUSR1 during prediction')
    parser.add_argument('--signal-stack', choices=('alternate', 'current'), default='alternate')
    parser.add_argument('--block-size', type=int, choices=(8, 16), default=16,
                        help='Shape of the inspected predictor entry; not inferred from its offset')
    args = parser.parse_args()
    if args.signal_stack == 'current' and not args.signal_stress:
        parser.error('--signal-stack current requires --signal-stress')
    library = args.library.resolve(strict=True)
    if library.stat().st_size > 256 * 1024 * 1024 or sha(library) != args.library_sha256.lower():
        parser.error('Library size/hash does not match the inspected ELF')
    if not args.runtime_library.startswith('/') or any(ord(c) < 32 for c in args.runtime_library):
        parser.error('Provide an absolute runtime library path without control characters')
    data = library.read_bytes()
    code = elf_segment(data, args.function_offset, 128, 5)
    elf_segment(data, args.version_offset, 8, 4)
    compiler = Path(shutil.which('gcc') or '/missing-gcc').resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        parser.error('Requires an x86_64-linux-gnu compiler')
    source = Path(__file__).with_name('native_vp8_predictor_probe.c')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'native_vp8_predictor_probe'
    manifest = output / 'native-vp8-predictor-build.json'
    inputs = dict(sourceSha256=sha(source), compilerSha256=sha(compiler),
                  librarySha256=sha(library), runtimeLibrary=args.runtime_library,
                  functionOffset=args.function_offset, versionOffset=args.version_offset,
                  blockSize=args.block_size, signalStress=args.signal_stress, signalStack=args.signal_stack,
                  functionBytesSha256=hashlib.sha256(code).hexdigest())
    inputs['signalFramesSha256']=sha(source.with_name('native_guest_signal_frames.h'))
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('inputs') == inputs and binary.is_file() and old.get('sha256') == sha(binary):
        record = old
        print('Reused verified predictor probe')
    else:
        command = [str(compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-DPROBE_LIBRARY=' + json.dumps(args.runtime_library),
                   '-DPROBE_PREDICTOR_OFFSET=' + hex(args.function_offset),
                   '-DPROBE_VERSION_OFFSET=' + hex(args.version_offset),
                   '-DPROBE_BLOCK_SIZE=' + str(args.block_size),
                   '-DPROBE_SIGNAL_STRESS=' + str(int(args.signal_stress)),
                   '-DPROBE_SIGNAL_SAME_STACK=' + str(int(args.signal_stack == 'current')),
                   str(source), '-ldl', '-pthread', '-o', str(binary)]
        subprocess.run(command, check=True, timeout=45)
        record = dict(inputs=inputs, sha256=sha(binary), bytes=binary.stat().st_size,
                      architecture='x86_64-linux-gnu', deviceExecuted=False)
    if args.host_check:
        result = subprocess.run([str(binary), str(library)], capture_output=True,
                                text=True, timeout=130)
        text = result.stdout + result.stderr
        record['hostCheck'] = dict(exitCode=result.returncode,
            passed=result.returncode == 0 and
                f'VP8_PREDICTOR complete cases={196608 if args.signal_stress else 24576} guard_mismatches=0 result=0' in text,
            events=[line for line in text.splitlines() if line.startswith('VP8_PREDICTOR')])
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({key: record[key] for key in ['sha256', 'bytes', 'architecture']}))
    if args.host_check and not record['hostCheck']['passed']:
        raise SystemExit('Host predictor check failed; inspect the manifest')


if __name__ == '__main__':
    main()
