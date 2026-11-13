#!/usr/bin/env python3
"""Export a verified local GNU/Steam resource directory, separately from the HAP.

This script does not download, execute, license, or publish Valve software.
Only an explicitly supplied local runtime is exported for device diagnostics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile


def sha(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.runtime / "manifest.json").read_text())
    client = manifest.get("steamClientProbe", {})
    if not client.get("fullClient") or not client.get("originalSteamUnmodified"):
        parser.error("Expected a complete original ARM64 diagnostic runtime")
    if "patches/glibc/0003-ohos-opt-in-external-native-memory.patch" not in manifest.get("gnuRuntime", {}).get("patches", {}):
        parser.error("External resources require the paired native-memory GNU loader/libc")
    if args.output.exists():
        parser.error("Output must be a new directory")
    names = {name: record["sha256"] for name, record in manifest["files"].items()}
    names["manifest.json"] = sha(args.runtime / "manifest.json")
    for probe, filename in [(client, "steam-client-data.zip"), (manifest["xwaylandProbe"], "xkb-data.zip")]:
        names[filename] = probe["data"]["sha256"]
        # Archives contain data only and no traversal, links or ELF payloads.
        with zipfile.ZipFile(args.runtime / filename) as archive:
            for item in archive.infolist():
                path = Path(item.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in item.filename or ((item.external_attr >> 16) & 0o170000) == 0o120000:
                    parser.error("Unsafe data archive member")
                if not item.is_dir():
                    with archive.open(item) as source:
                        if source.read(4) == b"\x7fELF":
                            parser.error("Executable in data archive")
    for name in ["COPYING.LIB", "LICENSES"]:
        if (args.runtime / name).is_file():
            names[name] = sha(args.runtime / name)
    for folder in ["wayland-notices", "xwayland-notices"]:
        for source in (args.runtime / folder).glob("*"):
            if source.is_file(): names[folder + "/" + source.name] = sha(source)
    records = []
    args.output.mkdir(parents=True)
    for name, digest in sorted(names.items()):
        source = args.runtime / name
        if Path(name).is_absolute() or ".." in Path(name).parts or source.is_symlink() or sha(source) != digest:
            parser.error("Invalid runtime file: " + name)
        destination = args.output / "runtime" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        records.append({"path": "runtime/" + name, "sha256": digest, "bytes": source.stat().st_size})
    payload = {"schemaVersion": 1, "layout": "SteamLinuxARM64", "executionModel": "native_aarch64",
               "steamSha256": client["steamSha256"], "files": records}
    (args.output / "manifest.json").write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"externalDirectory": str(args.output), "files": len(records),
                      "bytes": sum(item["bytes"] for item in records),
                      "manifestSha256": sha(args.output / "manifest.json")}))

if __name__ == "__main__":
    main()
