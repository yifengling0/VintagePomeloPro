"""Pin the preserved GL, Vulkan, input, network and FEX binary compatibility base.

Intentional upgrades require reviewing the lock alongside their regression
evidence. This gate does not infer compatibility from a self-reported version.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'docs/graphics/runtime-core.lock.json'


def packaged_gstreamer_files(inventory_sha256):
    record = json.loads(LOCK.read_text()).get('packagedNativeGStreamer', {})
    return record.get('files') if record.get('loaderInventorySha256') == inventory_sha256 else None


def validate_core(read, section='libraries', candidate=None, gpu_bc_upload=False):
    lock = json.loads(LOCK.read_text())
    if lock.get('schemaVersion') != 1 or not lock.get(section):
        raise ValueError('Missing core runtime compatibility lock: ' + section)
    expected_files = dict(lock[section])
    if gpu_bc_upload and section == 'libraries' and candidate is None:
        expected_files.update(lock['gpuBcUploadLibraries'])
    for name, expected in expected_files.items():
        if candidate is not None:
            from source_build_contract import validate_member
            validate_member(candidate, section, name, read(name))
            continue
        if hashlib.sha256(read(name)).hexdigest() != expected:
            raise ValueError('Unreviewed core runtime replacement: ' + name)
