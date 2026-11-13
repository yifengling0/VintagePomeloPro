#!/usr/bin/env python3
"""Replay every production FEX overlay on the exact gitlink, then replay again.

Only the small set of patch inputs is materialized; no full source/build copies
or device are needed. The real build script runs in patch-only mode. Its output
must be identical after an incremental invocation. Also compile the real VM
tracker from this integrated result to preserve behavioral coverage.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Git repository containing the pinned FEX objects')
    parser.add_argument('--output', type=Path, help='Small JSON evidence file')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = args.source or root / 'thirdparty/fex'
    pin = subprocess.check_output(['git', '-C', str(root), 'rev-parse',
        'HEAD:thirdparty/fex'], text=True).strip()
    paths = {'CMakeLists.txt', 'Data/CMake/toolchain_mingw.cmake',
        'FEXCore/include/FEXCore/Utils/IntervalList.h'}
    for patch in (root / 'scripts/patches').glob('fex-*.patch'):
        paths.update(re.findall(r'^--- a/(.+)$', patch.read_text(), re.M))
    for rel in paths:
        if Path(rel).is_absolute() or '..' in Path(rel).parts:
            raise ValueError(f'Unsafe patch path: {rel}')
    with tempfile.TemporaryDirectory(prefix='fex-overlay-replay-') as folder:
        fixture = Path(folder)
        for rel in sorted(paths):
            path = fixture / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(['git', '-C', str(source),
                'show', f'{pin}:{rel}']))
        env = os.environ.copy()
        env.update(WINE_ARCH='aarch64', FEX_SRC=str(fixture),
            FEX_PATCH_CHECK_ONLY='1', FEX_STEAM_DIAG_PATCHES='0',
            FEX_EXACTSTORE_DIAGNOSTICS='0', FEX_STRICT_MUL24='0')
        command = ['bash', str(root / 'scripts/build_fex.sh')]
        subprocess.run(command, env=env, check=True, timeout=180)
        def hashes():
            return {rel: hashlib.sha256((fixture / rel).read_bytes()).hexdigest()
                for rel in sorted(paths)}
        first = hashes()
        subprocess.run(command, env=env, check=True, timeout=180)
        if first != hashes():
            raise AssertionError('Incremental FEX replay changed the source')
        subprocess.run(['python3', str(root / 'host_tests/fex_vm_contract_test.py'),
            '--source', str(fixture)], check=True, timeout=120)
        report = {'pin': pin, 'files': len(paths), 'freshReplay': 'PASS',
            'incrementalReplay': 'PASS', 'vmTrackerBehavior': 'PASS',
            'sha256': first}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"PASS: FEX overlays on {pin}, {len(paths)} files, fresh + incremental + VM behavior")


if __name__ == '__main__':
    run()
