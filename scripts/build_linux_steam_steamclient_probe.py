#!/usr/bin/env python3
"""Build the native GNU read-only Steam helper, without executing Valve code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--host-reference", action="store_true")
    args = parser.parse_args()
    cc = Path(shutil.which(args.cc) or args.cc).resolve(strict=True)
    machine = subprocess.check_output([str(cc), "-dumpmachine"], text=True).strip()
    if "linux-gnu" not in machine or (not args.host_reference and not machine.startswith("aarch64")):
        parser.error("Expected a GNU ARM64 compiler; --host-reference is only for host tests")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    files = {}
    sources = {}
    for name, source in [("steamohos_gnu_steamclient", "steamclient_probe.c"),
                         ("steamohos_gnu_steam_session", "steamclient_session.c"),
                         ("compatibilitytools.d/hmosdeck/hmosdeck", "hmosdeck.cpp")]:
        path = SOURCES / source
        (output / name).parent.mkdir(parents=True, exist_ok=True)
        compiler = str(cc)[:-3] + 'g++' if source.endswith('.cpp') and str(cc).endswith('gcc') else str(cc)
        language = '-std=c++17' if source.endswith('.cpp') else '-std=c11'
        extra = [str(SOURCES / "steamclient_rpc.c"), str(SOURCES / "steamclient_identity.c")] if source == "steamclient_probe.c" else []
        subprocess.run([compiler, language, "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie",
                        "-Wl,-z,relro,-z,now", str(path), *extra, "-ldl", "-o", str(output / name)], check=True)
        binary = output / name
        files[name] = {"sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "bytes": binary.stat().st_size}
        sources[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    sources["entry/src/main/cpp/linux_steam/launch_wire.h"] = hashlib.sha256((SOURCES.parent / "launch_wire.h").read_bytes()).hexdigest()
    for name in ["steamclient_rpc.c", "steamclient_probe.h", "steamclient_identity.c"]:
        path = SOURCES / name
        sources[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = {"schemaVersion": 1, "compiler": str(cc), "target": machine,
                "executionModel": "native_aarch64" if machine.startswith("aarch64") else "native_host",
                "readOnly": False, "gameLaunchTool": "compatibilitytools.d/hmosdeck/hmosdeck", "steamApiInitVerified": False, "authenticationVerified": False,
                "files": files, "sources": sources}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"output": str(output), "target": machine, "files": list(files)}))


if __name__ == "__main__":
    main()
