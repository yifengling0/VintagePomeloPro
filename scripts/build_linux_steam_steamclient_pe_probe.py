#!/usr/bin/env python3
"""Build a Windows x64 diagnostic client for the read-only GNU Steam transport."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    cc = Path(os.path.abspath(shutil.which(args.cc) or args.cc))
    if not cc.is_file():
        parser.error("Compiler is missing")
    machine = subprocess.check_output([str(cc), "-dumpmachine"], text=True).strip()
    if not machine.startswith("x86_64") or not any(x in machine for x in ["mingw", "windows-gnu"]):
        parser.error("Expected an x86_64 Windows GNU/LLVM-MinGW compiler")
    if args.output.exists() or args.output.with_suffix(".json").exists():
        parser.error("Output and manifest must be new files")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    source = ROOT / "host_tests/linux_steam_steamclient_pe_probe.c"
    header = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/steamclient_probe.h"
    subprocess.run([str(cc), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static", str(source),
                    "-lws2_32", "-o", str(args.output)], check=True)
    manifest = {"schemaVersion": 1, "target": machine, "compiler": str(cc), "readOnly": True,
                "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(), "bytes": args.output.stat().st_size,
                "steamApiInitVerified": False, "authenticationVerified": False,
                "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [source, header]}}
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "target": machine, "bytes": manifest["bytes"]}))


if __name__ == "__main__":
    main()
