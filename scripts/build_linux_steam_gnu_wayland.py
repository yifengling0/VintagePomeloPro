#!/usr/bin/env python3
"""Build native GNU Wayland dependencies and extend an existing diagnostic runtime.

Only build-machine tools are executed. No QEMU/exe_wrapper is used. Inputs are
pinned upstream source archives; output is a new directory and remains untested.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVES = {
    "libffi": ("libffi-3.4.6", "b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e"),
    "wayland": ("wayland-1.22.0", "1540af1ea698a471c2d8e9d288332c7e0fd360c8f1d12936ebb7e7cbc2425842"),
}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--base-runtime", required=True, type=Path)
    parser.add_argument("--glibc-build", required=True, type=Path)
    parser.add_argument("--libffi-archive", required=True, type=Path)
    parser.add_argument("--wayland-archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    cc = args.cc.resolve(strict=True)
    cxx = cc.with_name(cc.name.replace("gcc", "g++"))
    target = subprocess.check_output([str(cc), "-dumpmachine"], text=True).strip()
    if not target.startswith("aarch64") or "linux-gnu" not in target:
        parser.error("Expected a native GNU/Linux AArch64 target compiler")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    work = output / "build"
    work.mkdir()
    environment = os.environ.copy()
    environment["PATH"] = str(cc.parent) + os.pathsep + environment["PATH"]
    archive_records = {}
    for name in ARCHIVES:
        archive = getattr(args, name + "_archive").resolve(strict=True)
        directory, expected = ARCHIVES[name]
        if sha(archive) != expected:
            parser.error("Pinned upstream archive mismatch: " + name)
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    parser.error("Unsafe archive path")
                if member.issym() or member.islnk():
                    destination = (work / member.name).parent / member.linkname
                    if not destination.resolve().is_relative_to(work):
                        parser.error("Unsafe archive link")
            package.extractall(work)
        archive_records[name] = {"sourceArchive": str(archive), "sha256": expected, "version": directory}
    def run(argv, cwd=None, env=environment):
        subprocess.run([str(a) for a in argv], cwd=cwd, env=env, check=True)
    prefix = work / "gnu-deps"
    ffi_build = work / "libffi-build"
    ffi_build.mkdir()
    ffi_env = environment | {"CC": str(cc), "CXX": str(cxx)}
    run([work / "libffi-3.4.6/configure", "--host=" + target, "--prefix=" + str(prefix),
         "--disable-docs", "--disable-multi-os-directory", "--disable-static"], ffi_build, ffi_env)
    run(["make", "-j" + str(args.jobs)], ffi_build)
    run(["make", "install"], ffi_build)
    native = work / "native-scanner"
    run(["meson", "setup", work / "scanner-build", work / "wayland-1.22.0", "--prefix=" + str(native),
         "--libdir=lib", "-Dlibraries=false", "-Dtests=false", "-Ddocumentation=false", "-Ddtd_validation=false"])
    run(["ninja", "-C", work / "scanner-build", "install"])
    cross = work / "cross.ini"
    cross.write_text("[binaries]\nc = '" + str(cc) + "'\nar = '" + str(cc.with_name(cc.name.replace('gcc', 'ar'))) +
        "'\nstrip = '" + str(cc.with_name(cc.name.replace('gcc', 'strip'))) + "'\npkgconfig = '/usr/bin/pkg-config'\n" +
        "[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n" +
        "[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = ['" + str(prefix / 'lib/pkgconfig') + "']\n")
    cross_env = environment | {"PKG_CONFIG_PATH_FOR_BUILD": str(native / "lib/pkgconfig")}
    run(["meson", "setup", work / "wayland-build", work / "wayland-1.22.0", "--cross-file=" + str(cross),
         "--prefix=" + str(prefix), "--libdir=lib", "-Dscanner=false", "-Dtests=false", "-Ddocumentation=false",
         "-Ddtd_validation=false"], env=cross_env)
    run(["ninja", "-C", work / "wayland-build", "install"], env=cross_env)
    runtime = output / "runtime"
    shutil.copytree(args.base_runtime.resolve(strict=True), runtime)
    manifest = json.loads((runtime / "manifest.json").read_text())
    for name, identity in manifest["files"].items():
        if sha(runtime / name) != identity["sha256"]:
            parser.error("Base GNU runtime manifest mismatch: " + name)
    cpp = ROOT / "entry/src/main/cpp"
    common = ["-O2", "-g0", "-Wall", "-Wextra", "-Werror", "-Wl,-z,relro,-z,now", "-Wl,--hash-style=both",
              "-I" + str(cpp), "-I" + str(prefix / "include")]
    protocol = work / "xdg-shell.o"
    run([cc, *common, "-fPIC", "-c", cpp / "protocols/xdg-shell-protocol.c", "-o", protocol])
    run([cxx, *common, "-std=c++17", "-fPIE", "-pie", cpp / "linux_steam/gnu_fixture/wayland.cpp",
         cpp / "linux_steam/surface_probe.cpp", protocol, "-L" + str(prefix / "lib"),
         "-Wl,-rpath-link," + str(prefix / "lib"), "-lwayland-client", "-pthread", "-o", runtime / "steamohos_gnu_wayland"])
    for name in ["libwayland-client.so.0", "libffi.so.8"]:
        shutil.copyfile((prefix / "lib" / name).resolve(strict=True), runtime / name)
    for name in ["libstdc++.so.6", "libgcc_s.so.1"]:
        original = Path(subprocess.check_output([str(cc), "-print-file-name=" + name], text=True).strip()).resolve(strict=True)
        shutil.copyfile(original, runtime / name)
    glibc_build = args.glibc_build.resolve(strict=True) / "build"
    if sha(glibc_build / "libc.so") != manifest["files"]["libc.so.6"]["sha256"]:
        parser.error("libm must come from the paired glibc build")
    shutil.copyfile((glibc_build / "math/libm.so.6").resolve(strict=True), runtime / "libm.so.6")
    extra = ["steamohos_gnu_wayland", "libwayland-client.so.0", "libffi.so.8", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6"]
    for name in extra:
        manifest["files"][name] = {"sha256": sha(runtime / name), "bytes": (runtime / name).stat().st_size}
        with (runtime / (name + ".readelf.txt")).open("w") as log:
            subprocess.run(["readelf", "-h", "-l", "-d", "-V", str(runtime / name)], stdout=log, check=True)
    notices = runtime / "wayland-notices"
    notices.mkdir()
    shutil.copyfile(work / "libffi-3.4.6/LICENSE", notices / "libffi-LICENSE")
    shutil.copyfile(work / "wayland-1.22.0/COPYING", notices / "wayland-COPYING")
    shutil.copyfile(cc.parent.parent / "license.txt", notices / "Arm-GNU-Toolchain-license.txt")
    manifest["waylandProbe"] = {"builtOnly": True, "archives": archive_records,
        "sources": {str(f.relative_to(ROOT)): sha(f) for f in [cpp / "linux_steam/gnu_fixture/wayland.cpp",
        cpp / "linux_steam/surface_probe.cpp", cpp / "linux_steam/surface_probe.h", cpp / "protocols/xdg-shell-protocol.c"]},
        "cxxRuntime": "GNU toolchain 13.3.rel1 bundled libstdc++/libgcc; see compiler source distribution for notices"}
    (runtime / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"runtime": str(runtime), "target": target, "builtOnly": True, "newFiles": extra}))

if __name__ == "__main__":
    main()
