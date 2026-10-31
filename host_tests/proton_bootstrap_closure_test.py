#!/usr/bin/env python3
"""Check real PE architecture, hashes and import closure against the base GE."""
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pe_modules',root/'scripts/prepare_linux_proton_modules.py')
pe = importlib.util.module_from_spec(spec);spec.loader.exec_module(pe)
raw = root/'entry/src/main/resources/rawfile/linux-proton'
manifest = json.loads((raw/'bootstrap/manifest.json').read_text())
base = json.loads((raw/'manifest.json').read_text())
available = {Path(r['path']).name.lower() for r in base['files'] if '/aarch64-windows/' in r['path']}
available.update(r['name'] for r in manifest['files'])
assert {r['name'] for r in manifest['files']} == {
    'psapi.dll','wevtsvc.dll','lsass.exe','wsock32.dll','mswsock.dll',
    'dwmapi.dll','dwrite.dll','uiautomationcore.dll','oleacc.dll'}
for row in manifest['files']:
    data = (raw/'bootstrap'/row['name']).read_bytes()
    assert len(data) == row['bytes'] and hashlib.sha256(data).hexdigest() == row['sha256']
    header = struct.unpack_from('<I',data,60)[0]
    assert struct.unpack_from('<H',data,header+4)[0] in (0xaa64,0xa641,0xa64e)
    assert not pe.imports(data) - available, (row['name'], pe.imports(data)-available)
print('PASS: pinned ARM64 PE bootstrap hashes and complete imports/forwarders in delivered GE closure')
