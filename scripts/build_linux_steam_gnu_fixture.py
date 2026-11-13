#!/usr/bin/env python3
"""Build a GNU fixture and copy its paired native loader/libc for local tests."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"


def run(argv):
    return subprocess.check_output([str(value) for value in argv], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--host-reference", action="store_true")
    parser.add_argument("--glibc-build", type=pathlib.Path, help="Output of build_linux_steam_glibc.py")
    args = parser.parse_args()
    cc = pathlib.Path(shutil.which(args.cc) or args.cc).resolve()
    machine = run([cc, "-dumpmachine"])
    if "linux-gnu" not in machine or (not args.host_reference and not machine.startswith("aarch64")):
        parser.error("Use a GNU/Linux AArch64 compiler (or --host-reference for a native Linux reference).")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    common = [cc, "-std=c11", "-O2", "-g0", "-Wall", "-Wextra", "-Werror",
              "-Wl,-z,relro,-z,now", "-Wl,--hash-style=both"]
    subprocess.run([str(x) for x in common + ["-fPIC", "-shared", "-ftls-model=initial-exec", SOURCE / "tls.c",
                   "-Wl,--version-script=" + str(SOURCE / "versions.map"),
                   "-Wl,-soname,libsteamohos_gnu_tls.so", "-o", output / "libsteamohos_gnu_tls.so"]], check=True)
    subprocess.run([str(x) for x in common + ["-fPIC", "-shared", "-ftls-model=global-dynamic", SOURCE / "late_tls.c",
                   "-Wl,-soname,libsteamohos_gnu_late.so", "-o", output / "libsteamohos_gnu_late.so"]], check=True)
    subprocess.run([str(x) for x in common + ["-fPIE", "-pie", SOURCE / "main.c", "-L" + str(output),
                   "-lsteamohos_gnu_tls", "-pthread", "-ldl", "-o", output / "steamohos_gnu_fixture"]], check=True)
    loader_name = "ld-linux-aarch64.so.1" if machine.startswith("aarch64") else "ld-linux-x86-64.so.2"
    libraries = {}
    for name in [loader_name, "libc.so.6"]:
        if args.glibc_build:
            original = (args.glibc_build / "build" / ("libc.so" if name == "libc.so.6" else "elf/ld.so")).resolve(strict=True)
        else:
            original = pathlib.Path(run([cc, "-print-file-name=" + name])).resolve(strict=True)
        shutil.copy2(original, output / name)
        libraries[name] = {"source": str(original), "sha256": hashlib.sha256(original.read_bytes()).hexdigest()}
    files = [p for p in output.iterdir() if p.is_file()]
    manifest = {"schemaVersion": 1, "compiler": str(cc), "target": machine,
                "compilerVersion": run([cc, "--version"]).splitlines()[0],
                "executionModel": "native_aarch64" if machine.startswith("aarch64") else "native_host", "builtOnly": True,
                "libraries": libraries,
                "sources": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in SOURCE.iterdir() if p.is_file()},
                "files": {p.name: {"sha256": hashlib.sha256(p.read_bytes()).hexdigest(), "bytes": p.stat().st_size}
                          for p in files}}
    if args.glibc_build:
        manifest["gnuRuntime"] = json.loads((args.glibc_build / "build-manifest.json").read_text())
        for name in ["COPYING.LIB", "LICENSES"]:
            shutil.copyfile(args.glibc_build / "glibc-2.38" / name, output / name)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for path in files:
        (output / (path.name + ".readelf.txt")).write_text(run(["readelf", "-h", "-l", "-d", "-r", "-V", path]) + "\n")
    print(json.dumps({"output": str(output), "target": machine, "files": list(manifest["files"])}))


if __name__ == "__main__":
    main()
