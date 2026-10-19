#!/usr/bin/env python3
"""Build opt-in, experimental GNU / Wine Steam bridge components only.

This does not install them, replace a game DLL, or enable game authentication.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from generate_linux_steam_bridge import generate
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--mingw", required=True, type=Path)
    parser.add_argument("--gnu-cc", required=True, type=Path)
    parser.add_argument("--gnu-cxx", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--host-reference", action="store_true")
    args = parser.parse_args()
    target = subprocess.check_output([str(args.gnu_cc), "-dumpmachine"], text=True).strip()
    if "linux-gnu" not in target or (not args.host_reference and not target.startswith("aarch64")):
        parser.error("GNU ARM64 compiler required (host build is only a test reference)")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "generated"
    generate(args.reference, args.mingw / "x86_64-w64-mingw32-clang++", generated)
    source = ROOT / "entry/src/main/cpp/linux_steam/steam_bridge"
    sdk = args.reference / "lsteamclient/steamworks_sdk_150"
    flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-misleading-indentation",
             "-isystem", str(sdk), "-I" + str(generated)]
    files = []
    for architecture in ["i686", "x86_64"]:
        dll = output / architecture / "lsteamclient.dll"
        dll.parent.mkdir(exist_ok=True)
        subprocess.run([str(args.mingw / (architecture + "-w64-mingw32-clang++")), *flags,
                        "-fms-extensions", "-shared", "-static", str(source / "pe.cpp"), "-lws2_32",
                        "-Wl,--kill-at", "-o", str(dll)], check=True)
        files.append(dll)
    identity = output / "identity.o"
    subprocess.run([str(args.gnu_cc), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-c",
                    str(source.parent / "gnu_fixture/steamclient_identity.c"), "-o", str(identity)], check=True)
    binary = output / "steamohos_gnu_steam_bridge"
    subprocess.run([str(args.gnu_cxx), *flags, "-fPIE", "-pie", "-Wl,-z,relro,-z,now", str(source / "server.cpp"),
                    str(identity), "-ldl", "-o", str(binary)], check=True)
    files.append(binary)
    manifest = {"schemaVersion": 1, "status": "experimental_not_enabled", "gnuTarget": target,
                "steamApiInitVerified": False, "authenticationVerified": False,
                "protonCommit": "e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6",
                "files": {str(p.relative_to(output)): {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in files},
                "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source.glob("*")) if p.is_file()}}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"status": manifest["status"], "target": target, "files": manifest["files"]}))

if __name__ == "__main__":
    main()
