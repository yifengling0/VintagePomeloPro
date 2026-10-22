#!/usr/bin/env python3
"""Build GNU ARM64 Steam diagnostics without executing guest code on the builder.

The original Steam binary is pinned and unchanged; private POSIX adaptations
are confined to a GNU preload library, never linked into the OHOS host.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import re
import zipfile
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STEAM_SHA256 = "bd059af3d99c8b9979a5218838cce9ff44bef4c5ccc0b399b333db0ae1b84a89"
# Chromium/NSS loads these modules by name; DT_NEEDED alone misses them.
DYNAMIC_STEAM_DEPENDENCIES = ("libXss.so.1", "libsoftokn3.so", "libfreebl3.so",
                              "libfreeblpriv3.so", "libnssckbi.so")

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--base-runtime", required=True, type=Path)
    parser.add_argument("--steam-root", required=True, type=Path)
    parser.add_argument("--steam-sha256", default=STEAM_SHA256,
                        help="Expected original Steam digest from the locally verified official package")
    parser.add_argument("--glibc-build", required=True, type=Path)
    parser.add_argument("--xwayland-build", required=True, type=Path)
    parser.add_argument("--client-deps", type=Path)
    parser.add_argument("--alsa-include", type=Path, help="ALSA SDK include root for the optional OHOS AudioBroker playback plugin")
    parser.add_argument("--lsof-build", type=Path, help="Optional verified OHOS live socket lsof build")
    parser.add_argument("--mesa-build", type=Path, help="Optional verified GNU Mesa Xlib/virpipe build output")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--helpers-package", type=Path, help="Optional checksum-locked official Linux ARM64 misc package")
    args = parser.parse_args()
    if args.mesa_build and not args.client_deps:
        parser.error("--mesa-build requires --client-deps for the GNU dependency closure")
    if args.lsof_build and not args.client_deps:
        parser.error("--lsof-build requires --client-deps")
    cc = args.cc.resolve(strict=True)
    target = subprocess.check_output([str(cc), "-dumpmachine"], text=True).strip()
    if not target.startswith("aarch64") or "linux-gnu" not in target:
        parser.error("Expected GNU AArch64 compiler")
    steam = args.steam_root / "steamrtarm64/steam"
    if not re.fullmatch(r"[a-f0-9]{64}", args.steam_sha256) or sha(steam) != args.steam_sha256:
        parser.error("Original ARM64 Steam identity mismatch")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = output / "runtime"
    shutil.copytree(args.base_runtime.resolve(strict=True), runtime)
    manifest = json.loads((runtime / "manifest.json").read_text())
    if "steamClientProbe" in manifest:
        parser.error("Expected a clean Xwayland base runtime")
    for name, record in manifest["files"].items():
        if sha(runtime / name) != record["sha256"]:
            parser.error("Base runtime mismatch: " + name)
    additions = {"steamrtarm64/steam": steam,
                 "libdl.so.2": args.glibc_build / "dlfcn/libdl.so",
                 "libpthread.so.0": args.glibc_build / "nptl/libpthread.so",
                 "librt.so.1": args.glibc_build / "rt/librt.so"}
    glibc_manifest = args.glibc_build.parent / "build-manifest.json"
    if glibc_manifest.is_file():
        paired = json.loads(glibc_manifest.read_text())
        if "patches/glibc/0003-ohos-opt-in-external-native-memory.patch" in paired.get("patches", {}):
            additions["ld-linux-aarch64.so.1"] = args.glibc_build / "elf/ld.so"
            additions["libc.so.6"] = args.glibc_build / "libc.so"
            manifest["gnuRuntime"] = paired
    if args.mesa_build:
        mesa = json.loads((args.mesa_build / "manifest.json").read_text())
        if mesa.get("executionModel") != "native_aarch64":
            parser.error("Expected native AArch64 Mesa build")
        for name, target_name in [("libGL.so.1", "libsteamohos_software_gl.so"),
                                  ("steamohos_gnu_glx", "steamohos_gnu_glx")]:
            source = args.mesa_build / name
            if sha(source) != mesa["files"][name]["sha256"] or source.stat().st_size != mesa["files"][name]["bytes"]:
                parser.error("Mesa build mismatch: " + name)
            additions[target_name] = source
        manifest["mesaBuild"] = mesa
    for name, source in additions.items():
        (runtime / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, runtime / name)
    fixture = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"
    deps = args.xwayland_build / "deps-root/usr"
    lib = deps / "lib/aarch64-linux-gnu"
    def run(argv):
        subprocess.run([str(x) for x in argv], check=True)
    common = ["-O2", "-Wall", "-Wextra", "-Wl,-z,relro,-z,now"]
    cxx = cc.with_name(cc.name.replace("gcc", "g++"))
    native = fixture.parent
    objects = []
    for source in [fixture / "posix_compat.c", fixture / "sysvipc_compat.c", fixture / "filesystem_compat.c", fixture / "popen_compat.c", fixture / "socket_metadata.c",
                   fixture / "exec_compat.cpp", fixture / "syscall_compat.S", native / "native_loader.cpp", native / "native_loader_entry.S"]:
        obj = output / (source.name + ".o")
        compiler = cc if source.suffix == ".c" else cxx
        language = ["-std=c++17"] if source.suffix == ".cpp" else []
        run([compiler, *common, *language, "-I" + str(deps / "include"), "-fPIC", "-c", source, "-o", obj])
        objects.append(obj)
    run([cxx, *common, "-shared", *objects, "-pthread", "-ldl",
         "-Wl,-soname,libsteamohos_posix.so", "-o", runtime / "libsteamohos_posix.so"])
    run([cc, *common, "-fPIE", "-pie", ROOT / "host_tests/linux_steam_sysvipc_test.c",
         "-o", runtime / "steamohos_gnu_sysvipc"])
    run([cc, *common, "-Werror", "-fPIE", "-pie", fixture / "graphics_loader.c", "-ldl",
         "-o", runtime / "steamohos_gnu_graphics_loader"])
    run([cc, *common, "-Werror", "-fPIE", "-pie", fixture / "connection_identity.c",
         "-ldl", "-o", runtime / "steamohos_gnu_connection_identity"])
    cxx = cc.with_name(cc.name.replace("gcc", "g++"))
    run([cxx, *common, "-Werror", "-std=c++17", "-fPIE", "-pie", "-I" + str(deps / "include"),
         fixture / "xlib.cpp", "-L" + str(lib), "-Wl,-rpath-link," + str(lib), "-lX11",
         "-o", runtime / "steamohos_gnu_xlib"])
    run([cc, *common, "-Werror", "-fPIE", "-pie", "-I" + str(deps / "include"),
         fixture / "desktop_session.c", "-L" + str(lib), "-Wl,-rpath-link," + str(lib), "-lX11",
         "-ldl", "-o", runtime / "steamohos_gnu_desktop"])
    for binary, source in [("steamohos_gnu_steamclient", "steamclient_probe.c"),
                           ("steamohos_gnu_steam_session", "steamclient_session.c")]:
        extra = [fixture / "steamclient_rpc.c", fixture / "steamclient_identity.c"] if source == "steamclient_probe.c" else []
        run([cc, *common, "-Werror", "-fPIE", "-pie", fixture / source, *extra, "-ldl", "-o", runtime / binary])
    run([cc, *common, "-Werror", "-shared", "-fPIC", fixture / "sdl_gamepad.c", "-pthread", "-ldl",
         "-o", runtime / "libsteamohos_gamepad.so"])
    tool = runtime / "compatibilitytools.d/hmosdeck/hmosdeck"
    tool.parent.mkdir(parents=True, exist_ok=True)
    cxx = str(cc)[:-3] + "g++" if str(cc).endswith("gcc") else str(cc)
    subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie",
                    str(fixture / "hmosdeck.cpp"), "-o", str(tool)], check=True)
    names = sorted([*additions, "libsteamohos_gamepad.so", "libsteamohos_posix.so", "steamohos_gnu_sysvipc", "steamohos_gnu_xlib", "steamohos_gnu_desktop", "steamohos_gnu_graphics_loader", "steamohos_gnu_connection_identity", "steamohos_gnu_steamclient", "steamohos_gnu_steam_session", "compatibilitytools.d/hmosdeck/hmosdeck"])
    for name in names:
        path = runtime / name
        data = path.read_bytes()
        if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != 183:
            parser.error("Expected ARM64 ELF: " + name)
        manifest["files"][name] = {"sha256": sha(path), "bytes": len(data)}
    data_info = None
    if args.client_deps:
        for name, relative in [("libresolv.so.2", "resolv/libresolv.so"), ("libutil.so.1", "login/libutil.so")]:
            source = args.glibc_build / relative
            shutil.copyfile(source, runtime / name)
            manifest["files"][name] = {"sha256": sha(runtime / name), "bytes": source.stat().st_size}
            names.append(name)
        official = args.steam_root / "steamrtarm64"
        original_hashes = {}
        for source in sorted(official.rglob("*")):
            if not source.is_file():
                continue
            with source.open("rb") as stream:
                header = stream.read(20)
            if header[:4] != b"\x7fELF":
                continue
            if int.from_bytes(header[18:20], "little") != 183:
                parser.error("Non-ARM64 official code: " + str(source))
            name = "steamrtarm64/" + source.relative_to(official).as_posix()
            target = runtime / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            manifest["files"][name] = {"sha256": sha(target), "bytes": target.stat().st_size}
            original_hashes[name] = sha(target)
            if name not in names:
                names.append(name)
        search = [args.client_deps / "usr/lib/aarch64-linux-gnu", args.client_deps / "lib/aarch64-linux-gnu",
                  args.client_deps / "usr/lib/aarch64-linux-gnu/pulseaudio",
                  official, official / "libs", official / "video"]
        # Include dlopen modules and walk their own dependency closure below.
        for soname in DYNAMIC_STEAM_DEPENDENCIES:
            source = next((directory / soname for directory in search if (directory / soname).is_file()), None)
            if source is None:
                parser.error("Missing dynamic Steam dependency: " + soname)
            shutil.copyfile(source, runtime / soname)
            manifest["files"][soname] = {"sha256": sha(runtime / soname), "bytes": (runtime / soname).stat().st_size}
            names.append(soname)
        if args.alsa_include:
            if not (args.alsa_include / "alsa/pcm_ioplug.h").is_file():
                parser.error("Missing ALSA ioplug SDK headers")
            plugin = "libasound_module_pcm_hmosdeck.so"
            run([cc, *common, "-Werror", "-DPIC", "-shared", "-fPIC", "-I" + str(args.alsa_include),
                 fixture / "alsa_hmosdeck.c", "-L" + str(runtime), "-l:libasound.so.2", "-o", runtime / plugin])
            for name in [plugin]:
                manifest["files"][name] = {"sha256": sha(runtime/name), "bytes": (runtime/name).stat().st_size}
                names.append(name)
        # /bin/sh is absent in the app sandbox. Keep the checksum-locked native
        # GNU shell outside the HAP, with its dependency closure and notices.
        shell=args.client_deps / "bin/bash"
        if not shell.is_file():parser.error("Missing native GNU shell in client deps (Debian bash)")
        shutil.copyfile(shell,runtime / "steamohos_shell")
        manifest["files"]["steamohos_shell"]={"sha256":sha(runtime / "steamohos_shell"),"bytes":shell.stat().st_size}
        names.append("steamohos_shell")
        taskset=args.client_deps / "usr/bin/taskset"
        if not taskset.is_file():parser.error("Missing native GNU taskset in client deps (Debian util-linux)")
        shutil.copyfile(taskset,runtime / "steamohos_taskset")
        manifest["files"]["steamohos_taskset"]={"sha256":sha(runtime / "steamohos_taskset"),"bytes":taskset.stat().st_size}
        names.append("steamohos_taskset")
        lsof=args.client_deps / "usr/bin/lsof"
        if args.lsof_build:
            lsof_manifest=json.loads((args.lsof_build / "manifest.json").read_text())
            lsof=args.lsof_build / "steamohos_lsof"
            if (lsof_manifest.get("executionModel")!="native_aarch64" or
                lsof_manifest.get("adapted") is not True or
                lsof_manifest["files"]["steamohos_lsof"]!={"sha256":sha(lsof),"bytes":lsof.stat().st_size}):
                parser.error("Adapted lsof build identity mismatch")
            manifest["lsofBuild"]=lsof_manifest
        if not lsof.is_file():parser.error("Missing native GNU lsof in client deps (Debian lsof)")
        shutil.copyfile(lsof,runtime / "steamohos_lsof")
        manifest["files"]["steamohos_lsof"]={"sha256":sha(runtime / "steamohos_lsof"),"bytes":lsof.stat().st_size}
        names.append("steamohos_lsof")
        notices=[]
        for package_name in ["bash","libtinfo6","util-linux","lsof","libtirpc3"]:
            notice=args.client_deps / "usr/share/doc" / package_name / "copyright"
            if not notice.is_file():parser.error("Missing GNU shell notice: "+package_name)
            notices.append(package_name+"\n"+notice.read_text())
        if args.lsof_build:
            notices.append("lsof is an adapted OHOS build with an opt-in live descriptor backend. "
                           "Source and patch identities are recorded in manifest.json lsofBuild.")
        (runtime / "COPYING.shell").write_text("\n\n".join(notices))
        pending = ["steamohos_shell", "steamohos_taskset", "steamohos_lsof", *DYNAMIC_STEAM_DEPENDENCIES, "steamrtarm64/steamclient.so", "steamrtarm64/steamui.so", "steamrtarm64/steamwebhelper",
                   "steamrtarm64/vgui2_s.so"] + (["libsteamohos_software_gl.so", "steamohos_gnu_glx"] if args.mesa_build else [])
        closure = {}
        missing = set()
        while pending:
            name = pending.pop()
            if name in closure:
                continue
            dynamic = subprocess.check_output(["readelf", "-d", str(runtime / name)], text=True)
            needs = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
            closure[name] = needs
            for need in needs:
                local = "steamrtarm64/" + need
                if need in manifest["files"]:
                    resolved = need
                elif local in manifest["files"]:
                    resolved = local
                else:
                    source = next((d / need for d in search if (d / need).is_file()), None)
                    if source is None:
                        missing.add(need)
                        continue
                    target = runtime / need
                    shutil.copyfile(source, target)
                    header = target.read_bytes()[:20]
                    if header[:4] != b"\x7fELF" or int.from_bytes(header[18:20], "little") != 183:
                        parser.error("Invalid GNU dependency: " + need)
                    manifest["files"][need] = {"sha256": sha(target), "bytes": target.stat().st_size}
                    names.append(need)
                    resolved = need
                pending.append(resolved)
        if missing:
            parser.error("Missing Steam GNU dependencies: " + ", ".join(sorted(missing)))
        # Use checksum-locked Mozilla roots from the privately extracted Debian
        # ca-certificates data package, without running package maintainer scripts.
        certificate_files = sorted((args.client_deps / "usr/share/ca-certificates/mozilla").glob("*.crt"))
        if not certificate_files:
            parser.error("Missing trusted Mozilla CA certificate data")
        certificates = []
        for certificate in certificate_files:
            content = certificate.read_bytes()
            if content.count(b"-----BEGIN CERTIFICATE-----") != 1 or content.count(b"-----END CERTIFICATE-----") != 1:
                parser.error("Invalid CA certificate: " + certificate.name)
            certificates.append(content.rstrip() + b"\n")
        ca_bundle = b"".join(certificates)
        # All writable resources are data only. GNU code stays in signed native entries.
        data = runtime / "steam-client-data.zip"
        with zipfile.ZipFile(data, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=3) as package:
            for source in sorted(args.steam_root.rglob("*")):
                if not source.is_file() or source.is_symlink():
                    continue
                with source.open("rb") as stream:
                    if stream.read(4) == b"\x7fELF":
                        continue
                relative = source.relative_to(args.steam_root).as_posix()
                if relative.startswith("/") or ".." in Path(relative).parts or "\\" in relative:
                    parser.error("Unsafe Steam data path")
                package.write(source, relative)
            certificate_entry = zipfile.ZipInfo("steamohos/ca-certificates.crt", date_time=(1980, 1, 1, 0, 0, 0))
            certificate_entry.compress_type = zipfile.ZIP_DEFLATED
            certificate_entry.external_attr = 0o100644 << 16
            package.writestr(certificate_entry, ca_bundle)
        data_info = {"file": data.name, "sha256": sha(data), "bytes": data.stat().st_size,
                     "originalElfSha256": original_hashes, "closure": closure,
                     "trustedRoots": {"count": len(certificates), "sha256": hashlib.sha256(ca_bundle).hexdigest(),
                                      "files": {f.name: sha(f) for f in certificate_files}}}
    sources = [fixture / "posix_compat.c", fixture / "sysvipc_compat.c", fixture / "filesystem_compat.c", fixture / "popen_compat.c",
               fixture / "exec_compat.cpp", fixture / "syscall_compat.S", fixture / "socket_metadata.c", fixture / "xlib.cpp", fixture / "desktop_session.c", fixture / "deck_session_control.h", fixture / "deck_action_uri.h", fixture / "wine_window_bridge.h", fixture / "pointer_observer.h", fixture / "graphics_loader.c", fixture / "connection_identity.c", fixture / "steamclient_probe.c", fixture / "steamclient_session.c", fixture / "steamclient_rpc.c", fixture / "steamclient_identity.c", fixture / "steamclient_probe.h", fixture / "hmosdeck.cpp", fixture / "sdl_gamepad.c", fixture / "sdl3_virtual_abi.h", fixture / "alsa_hmosdeck.c", fixture / "hmosdeck-asound.conf", native / "launch_wire.h", native / "native_loader.cpp",
               native / "native_loader.h", native / "native_loader_entry.S",
               ROOT / "host_tests/linux_steam_sysvipc_test.c", Path(__file__)]
    manifest["steamClientProbe"] = {"files": sorted(names),
        "executables": ["steamrtarm64/steam", "steamohos_gnu_xlib", "steamohos_gnu_desktop", "steamohos_gnu_sysvipc", "steamohos_gnu_graphics_loader", "steamohos_gnu_connection_identity", "steamohos_gnu_steamclient", "steamohos_gnu_steam_session", "compatibilitytools.d/hmosdeck/hmosdeck"] +
            (["steamohos_gnu_glx"] if args.mesa_build else []) + (["steamohos_shell", "steamohos_taskset", "steamohos_lsof"] if args.client_deps else []),
        "fullClient": bool(args.client_deps), "data": data_info,
        "steamSha256": args.steam_sha256, "originalSteamUnmodified": True,
        "sources": {str(p.relative_to(ROOT)): sha(p) for p in sources},
        "compatibilityScope": "private X11/tmp/cert paths; bounded SysV IPC; single-thread signed GNU exec handoff", "uiVerified": False}
    (runtime / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.helpers_package:
        from import_linux_steam_client_helpers import import_helpers
        import_helpers(args.helpers_package, runtime)
    print(json.dumps({"runtime": str(runtime), "files": len(manifest["files"]), "steamUnmodified": True}))

if __name__ == "__main__":
    main()
