#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stage the real OHOS ELF dependency closure for bounded device qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SYSTEM = {'libc.so', 'libm.so', 'libdl.so', 'libpthread.so', 'librt.so',
          'libEGL.so', 'libGLESv3.so', 'libvulkan.so', 'libnative_window.so',
          'libnative_buffer.so', 'libnative_image.so', 'libhilog_ndk.z.so'}


def stage(sdk, shared):
    work = ROOT/'build/gamescope/arm64-v8a'
    output = work/'loader-runtime'
    output.mkdir(exist_ok=True)
    roots = [work/'install/lib', work/'deps/usr/lib', shared/'lib',
             shared.parents[2]/'native_arm64-v8a/libffi/install/lib',
             sdk/'llvm/lib/aarch64-linux-ohos']
    roots.extend(sorted((shared.parents[2]/'native_arm64-v8a').glob('*/install/lib')))
    roots.append(ROOT/'entry/libs/arm64-v8a')
    reader = sdk/'llvm/bin/llvm-readelf'
    pending = ['libgamescope_core.so', 'libc++_shared.so']
    records = {}
    while pending:
        name = pending.pop()
        if name in records or name in SYSTEM:
            continue
        path = next((p/name for p in roots if (p/name).is_file()), None)
        if path is None:
            raise ValueError('Missing OHOS dependency: '+name)
        header = subprocess.check_output([reader, '-h', path], text=True)
        dynamic = subprocess.check_output([reader, '-d', path], text=True)
        needed = re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic)
        if ('AArch64' not in header or any(x in dynamic for x in
                ('libc.so.6', 'libstdc++', 'ld-linux'))):
            raise ValueError('Non-OHOS dependency: '+str(path))
        target = output/name
        data = path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != sha:
            shutil.copyfile(path, target)
        records[name] = {'source': str(path), 'bytes': len(data), 'sha256': sha,
                         'needed': needed}
        pending.extend(needed)
    shutil.copy2(work/'gamescope-load-probe', output/'gamescope-load-probe')
    report = {'libraries': records, 'systemLibraries': sorted(SYSTEM),
              'bytes': sum(r['bytes'] for r in records.values()),
              'runtimeQualified': False}
    (work/'loader-runtime.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'files': len(records)+1, 'bytes': report['bytes']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--host-prefix', type=Path, required=True)
    args = parser.parse_args()
    stage(args.sdk.resolve(), args.host_prefix.resolve())
