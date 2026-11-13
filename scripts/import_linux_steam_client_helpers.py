#!/usr/bin/env python3
"""Import only checksum-locked PE helpers shipped by official Linux ARM64 Steam.

Consumes a user-supplied official package. No Windows Steam installation or
automatic download is used. The small external archive never contains account,
configuration, game files, Steam DLLs or GNU executables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "entry/src/main/cpp/linux_steam/client_helpers.lock.json"


def import_helpers(package, runtime, lock=None):
    lock = json.loads(LOCK.read_text()) if lock is None else lock
    if hashlib.sha1(package.read_bytes()).hexdigest() != lock["packageSha1"]:
        raise ValueError("Official Linux helper package digest mismatch")
    expected = {"legacycompat/iscriptevaluator.exe", "bin/d3ddriverquery64.exe"}
    if set(lock["files"]) != expected:
        raise ValueError("Unexpected client helper allowlist")
    contents = {}
    with zipfile.ZipFile(package) as source:
        for name, record in lock["files"].items():
            entries = [item for item in source.infolist() if item.filename == name]
            if len(entries) != 1 or entries[0].file_size != record["bytes"] or record["bytes"] > 1048576:
                raise ValueError("Missing, duplicate or oversized Linux helper: " + name)
            data = source.read(entries[0])
            if hashlib.sha256(data).hexdigest() != record["sha256"]:
                raise ValueError("Official Linux helper digest mismatch: " + name)
            pe = int.from_bytes(data[60:64], "little") if data[:2] == b"MZ" else 0
            if pe < 64 or data[pe:pe + 6] != b"PE\0\0\x64\x86":
                raise ValueError("Expected x64 Windows helper from Linux package: " + name)
            contents[name] = data
    manifest_path = runtime / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    archive = runtime / "steam-client-helpers.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as destination:
        for name, data in sorted(contents.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            destination.writestr(entry, data)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    # The native files map is exclusively GNU ARM64 ELF. PE helper data has its
    # own archive declaration and remains external, like steam-client-data.zip.
    manifest["files"].pop(archive.name, None)
    manifest["steamClientProbe"]["helpers"] = {
        "file": archive.name, "sha256": digest, "provenance": lock,
        "files": [{"path": name, **record} for name, record in lock["files"].items()]
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return {"archive": archive.name, "bytes": archive.stat().st_size, "sha256": digest,
            "helpers": list(contents), "source": lock["package"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(import_helpers(args.package, args.runtime)))


if __name__ == "__main__":
    main()
