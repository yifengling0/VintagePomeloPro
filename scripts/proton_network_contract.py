# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared build/package audit for the patched GE Unix network module."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native_path(name):
    if os.name == 'nt':
        match = re.fullmatch(r'/mnt/([a-z])/(.*)', name)
        if match:
            return Path(match[1].upper() + ':/' + match[2])
        if name.startswith('/'):
            return None
    return Path(name)


def validate_network_build(receipt_path, root=ROOT):
    receipt_path = Path(receipt_path)
    receipt = json.loads(receipt_path.read_text())
    lock_path = root / 'docs/linux-steam/gnu-proton-network.lock.json'
    lock = json.loads(lock_path.read_text())
    if (receipt.get('wineRevision') != lock['wineRevision'] or
            receipt.get('unixCallTables') != {'__wine_unix_call_funcs': lock['unixCallSlots']}):
        raise ValueError('GNU NSI version or Unix-call ABI mismatch')
    required = [lock_path, root / 'scripts/build_linux_proton_network.py',
                root / 'scripts/proton_network_contract.py',
                *[root / row['path'] for row in lock['patches']]]
    inputs = receipt.get('inputs', {})
    dependencies = receipt.get('dependencies', {})
    if not inputs or not dependencies:
        raise ValueError('Missing GNU NSI build identity')
    for path in required:
        found = [value for name, value in inputs.items()
                 if native_path(name) is not None and native_path(name).resolve() == path.resolve()]
        if found != [digest(path)]:
            raise ValueError('GNU NSI missing or stale build input: ' + str(path))
    linux_inputs = {}
    for path, expected in {**inputs, **dependencies}.items():
        mapped = native_path(path)
        if mapped is None:
            linux_inputs[path] = expected
            continue
        if not mapped.is_file() or digest(mapped) != expected:
            raise ValueError('Stale GNU NSI input: ' + path)
    if linux_inputs:
        code = ('import sys,json,hashlib; from pathlib import Path; rows=json.load(sys.stdin); '
                'print(json.dumps([name for name,expected in rows.items() '
                'if not Path(name).is_file() or hashlib.sha256(Path(name).read_bytes()).hexdigest()!=expected]))')
        checked = subprocess.run(['wsl.exe', '-e', 'python3', '-c', code], input=json.dumps(linux_inputs),
                                 capture_output=True, text=True, encoding='utf-8', errors='replace',
                                 timeout=60, check=True)
        if json.loads(checked.stdout):
            raise ValueError('Stale GNU NSI WSL dependencies')
    output = receipt_path.parent / 'nsiproxy.so'
    data = output.read_bytes()
    if (not 0 < len(data) <= 1024 * 1024 or len(data) != receipt['bytes'] or
            hashlib.sha256(data).hexdigest() != receipt['sha256'] or
            data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00'):
        raise ValueError('GNU NSI output identity or architecture mismatch')
    return output, receipt
