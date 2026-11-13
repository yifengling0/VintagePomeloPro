#!/usr/bin/env python3
"""Build a native GNU Xwayland research runtime over the existing Wayland host.

Extracts checksum-locked Debian ARM64 development packages into a private
prefix, builds upstream Xwayland/xkbcomp, and records the ELF dependency closure.
Never installs host packages or runs AArch64 binaries on the build machine.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "xwayland-24.1.3.tar.xz": "dcdb57a66cc9b124c8f936760592628ac4e744a7d7b3179aa86189ad7ea4cb10",
    "xorgproto-2024.1.tar.xz": "372225fd40815b8423547f5d890c5debc72e88b91088fbfb13158c20495ccb59",
    "wayland-protocols-1.36.tar.xz": "71fd4de05e79f9a1ca559fac30c1f8365fa10346422f9fe795f74d77b9ef7e92",
    "xkbcomp-1.4.7.tar.xz": "0a288114e5f44e31987042c79aecff1ffad53a8154b8ec971c24a69a80f81f77",
}
DEVICE_XKB = "/data/storage/el2/base/files/steamohos-native/xkb-data"

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def package_x11_data(prefix, output):
    # Keep the historical archive name; it carries X11 keyboard and locale data.
    # libX11's wide-text APIs require the real XLC database, even for C locale.
    locales = prefix / "share/X11/locale"
    if not (locales / "C/XLC_LOCALE").is_file() or not (locales / "locale.dir").is_file():
        raise RuntimeError("Pinned libx11-data is incomplete")
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for source, root in [(prefix / "share/X11/xkb", "xkb-data"), (locales, "x11-locale")]:
            for file in sorted(source.rglob("*")):
                if file.is_file():
                    archive.write(file, str(Path(root) / file.relative_to(source)))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--base-runtime", required=True, type=Path)
    parser.add_argument("--wayland-build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=12)
    args = parser.parse_args()
    cc = args.cc.resolve(strict=True)
    target = subprocess.check_output([str(cc), "-dumpmachine"], text=True).strip()
    if not target.startswith("aarch64") or "linux-gnu" not in target:
        parser.error("Expected GNU AArch64 compiler")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    deps = out / "deps-root"
    prefix = deps / "usr"
    lib = prefix / "lib/aarch64-linux-gnu"
    lock = json.loads((args.artifacts / "debian-packages.lock.json").read_text())
    index = args.artifacts / "bookworm-arm64-Packages.xz"
    if sha(index) != lock["packagesIndexSha256"]:
        parser.error("Debian package index mismatch")
    env = os.environ.copy()
    env["PATH"] = str(cc.parent) + os.pathsep + env["PATH"]
    env["PKG_CONFIG_LIBDIR"] = str(lib / "pkgconfig") + ":" + str(prefix / "share/pkgconfig")
    env["PKG_CONFIG_PATH"] = ""
    def run(argv, cwd=None, environment=env):
        subprocess.run([str(x) for x in argv], cwd=cwd, env=environment, check=True)
    for name, record in lock["packages"].items():
        archive = args.artifacts / "debs" / Path(record["Filename"]).name
        if sha(archive) != record["SHA256"] or record["Architecture"] not in ["arm64", "all"]:
            parser.error("Debian package mismatch: " + name)
        run(["dpkg-deb", "-x", archive, deps])
    for pc in (prefix / "lib").rglob("*.pc"):
        if not pc.is_symlink():
            pc.write_text(pc.read_text().replace("/usr", str(prefix)))
    for path in deps.rglob("*"):
        if path.is_symlink():
            destination = os.readlink(path)
            if destination.startswith("/"):
                path.unlink()
                path.symlink_to(os.path.relpath(deps / destination.lstrip("/"), path.parent))
    wayland = args.wayland_build.resolve(strict=True) / "build"
    base_deps = wayland / "gnu-deps"
    for kind in ["include", "lib"]:
        for file in (base_deps / kind).rglob("*"):
            if not file.is_file():
                continue
            destination = (prefix / "include" if kind == "include" else lib) / file.relative_to(base_deps / kind)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if file.suffix == ".pc":
                destination.write_text(file.read_text().replace(str(base_deps), str(prefix)).replace("${prefix}/lib", "${prefix}/lib/aarch64-linux-gnu"))
            else:
                shutil.copyfile(file, destination)
    for name, expected in SOURCES.items():
        archive = args.artifacts / name
        if sha(archive) != expected:
            parser.error("Pinned upstream source mismatch: " + name)
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    parser.error("Unsafe upstream archive path")
                if member.issym() or member.islnk():
                    if not ((out / member.name).parent / member.linkname).resolve().is_relative_to(out):
                        parser.error("Unsafe upstream archive link")
            package.extractall(out)
    for name in ["xorgproto-2024.1", "wayland-protocols-1.36"]:
        options = ["-Dtests=false"] if name.startswith("wayland") else []
        build = out / ("build-" + name)
        run(["meson", "setup", build, out / name, "--prefix=" + str(prefix), "--libdir=lib/aarch64-linux-gnu", *options])
        run(["ninja", "-C", build, "install"])
    source = out / "xkbcomp-1.4.7"
    build = out / "build-xkbcomp"
    build.mkdir()
    cenv = env | {"CC": str(cc), "CFLAGS": "-O2 -fPIC", "CPPFLAGS": "-I" + str(prefix / "include"),
                  "LDFLAGS": "-L" + str(lib) + " -Wl,-rpath-link," + str(lib) + ":" + str(deps / "lib/aarch64-linux-gnu")}
    run([source / "configure", "--host=" + target, "--prefix=" + str(prefix),
         "--with-xkb-config-root=" + DEVICE_XKB], build, cenv)
    names = ["action", "alias", "compat", "expr", "geometry", "indicators", "keycodes", "keymap", "keytypes",
             "listing", "misc", "parseutils", "symbols", "utils", "vmod", "xkbcomp", "xkbparse", "xkbpath", "xkbscan"]
    objects = []
    for name in names:
        obj = build / (name + ".o")
        objects.append(obj)
        run([cc, "-O2", "-fPIC", "-DHAVE_CONFIG_H", "-Dmain=SteamOhosXkbcompMain",
             '-DDFLT_XKB_CONFIG_ROOT="' + DEVICE_XKB + '"', "-I" + str(build), "-I" + str(source),
             "-I" + str(prefix / "include"), "-c", source / (name + ".c"), "-o", obj])
    (build / "exports.map").write_text("{ global: SteamOhosXkbcompMain; local: *; };\n")
    run([cc, "-shared", "-Wl,-z,relro,-z,now", "-Wl,--version-script=" + str(build / "exports.map"),
         "-Wl,-soname,libsteamohos_xkbcomp.so", *objects, "-L" + str(lib), "-Wl,-rpath-link," + str(lib),
         "-lxkbfile", "-lX11", "-o", lib / "libsteamohos_xkbcomp.so"])
    source = out / "xwayland-24.1.3"
    patch = ROOT / "patches/xwayland/0001-ohos-native-keymap-compiler.patch"
    credential_patch = ROOT / "patches/xwayland/0002-ohos-query-credentials-without-changing-identity.patch"
    window_patch = ROOT / "patches/xwayland/0003-ohos-window-failure-diagnostics.patch"
    keymap_patch = ROOT / "patches/xwayland/0004-ohos-keymap-temporary-file-diagnostics.patch"
    for change in [patch, credential_patch, window_patch, keymap_patch]:
        run(["patch", "-p1", "--input=" + str(change)], source)
    cross = out / "cross.ini"
    cross.write_text("[binaries]\nc = '" + str(cc) + "'\nar = '" + str(cc.with_name(cc.name.replace('gcc', 'ar'))) +
        "'\nstrip = '" + str(cc.with_name(cc.name.replace('gcc', 'strip'))) + "'\npkgconfig = '/usr/bin/pkg-config'\n" +
        "[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n" +
        "[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = ['" + str(lib / 'pkgconfig') + "', '" +
        str(prefix / 'share/pkgconfig') + "']\n[built-in options]\nc_args = ['-I" + str(prefix / 'include') +
        "', '-I" + str(prefix / "include/libdrm") + "', '-DSTEAMOHOS_NATIVE_XKBCOMP=1', '-D_GNU_SOURCE=1']\nc_link_args = ['-L" + str(lib) + "', '-Wl,-rpath-link," +
        str(lib) + ":" + str(deps / "lib/aarch64-linux-gnu") + "', '-ldl']\n")
    env["PKG_CONFIG_PATH_FOR_BUILD"] = str(wayland / "native-scanner/lib/pkgconfig")
    options = ["-Dglamor=false", "-Dglx=false", "-Ddrm=false", "-Ddri3=false", "-Dxvfb=false",
        "-Dxwayland_ei=false", "-Dlibdecor=false", "-Dsystemd_notify=false", "-Dinput_thread=false",
        "-Dxselinux=false", "-Dsecure-rpc=false", "-Dxdmcp=false", "-Ddocs=false", "-Ddevel-docs=false",
        "-Dsha1=libmd", "-Ddefault_font_path=built-ins", "-Dxkb_dir=" + DEVICE_XKB, "-Dxkb_output_dir=/nonexistent"]
    build = out / "build-xwayland"
    run(["meson", "setup", build, source, "--cross-file=" + str(cross), "--prefix=" + str(prefix), "--wrap-mode=nofallback", *options])
    run(["ninja", "-C", build, "-j" + str(args.jobs)])
    runtime = out / "runtime"
    shutil.copytree(args.base_runtime.resolve(strict=True), runtime)
    manifest = json.loads((runtime / "manifest.json").read_text())
    for name, record in manifest["files"].items():
        if sha(runtime / name) != record["sha256"]:
            parser.error("Base runtime mismatch: " + name)
    shutil.copyfile(build / "hw/xwayland/Xwayland", runtime / "Xwayland")
    shutil.copyfile(lib / "libsteamohos_xkbcomp.so", runtime / "libsteamohos_xkbcomp.so")
    client = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/x11.cpp"
    cxx = cc.with_name(cc.name.replace("gcc", "g++"))
    run([cxx, "-O2", "-Wall", "-Wextra", "-Werror", "-std=c++17", "-fPIE", "-pie", "-Wl,-z,relro,-z,now",
         "-I" + str(prefix / "include"), client, "-L" + str(lib), "-Wl,-rpath-link," + str(lib), "-lxcb", "-o", runtime / "steamohos_gnu_x11"])
    pending = ["Xwayland", "libsteamohos_xkbcomp.so", "steamohos_gnu_x11"]
    extra = set(pending)
    closure = {}
    while pending:
        name = pending.pop()
        path = runtime / name
        data = path.read_bytes()
        if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], 'little') != 183:
            parser.error("Non-AArch64 ELF dependency: " + name)
        dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
        required = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        closure[name] = required
        for need in required:
            if need in manifest["files"] or need in extra:
                continue
            original = next((directory / need for directory in [lib, deps / "lib/aarch64-linux-gnu"]
                             if (directory / need).is_file()), None)
            if original is None:
                parser.error("Unresolved GNU dependency: " + need)
            shutil.copyfile(original, runtime / need)
            extra.add(need)
            pending.append(need)
    for name in sorted(extra):
        file = runtime / name
        manifest["files"][name] = {"sha256": sha(file), "bytes": file.stat().st_size}
    package_x11_data(prefix, runtime / "xkb-data.zip")
    notices = runtime / "xwayland-notices"
    notices.mkdir()
    for file in (prefix / "share/doc").glob("*/copyright"):
        shutil.copyfile(file, notices / (file.parent.name + '-copyright'))
    for name in ["xwayland-24.1.3", "xkbcomp-1.4.7", "xorgproto-2024.1", "wayland-protocols-1.36"]:
        file = out / name / "COPYING"
        if file.is_file():
            shutil.copyfile(file, notices / (name + '-COPYING'))
    manifest["xwaylandProbe"] = {"builtOnly": True, "version": "24.1.3", "sources": SOURCES,
        "patches": {str(p.relative_to(ROOT)): sha(p) for p in [patch, credential_patch, window_patch, keymap_patch]}, "debianPackages": lock,
        "files": sorted(extra), "executables": ["Xwayland", "steamohos_gnu_x11"], "closure": closure,
        "clientSourceSha256": sha(client), "buildScriptSha256": sha(Path(__file__)),
        "data": {"file": "xkb-data.zip", "sha256": sha(runtime / "xkb-data.zip"), "deviceParent": DEVICE_XKB.rsplit('/', 1)[0],
                 "includes": ["xkb-data", "x11-locale"]},
        "glamor": False, "glx": False, "renderer": "SHM diagnostic only"}
    (runtime / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"runtime": str(runtime), "nativeElfFiles": len(manifest["files"]), "builtOnly": True}))

if __name__ == "__main__":
    main()
