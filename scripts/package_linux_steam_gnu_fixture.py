#!/usr/bin/env python3
"""Prepare an unsigned local diagnostic HAP; official signing must follow."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hap", type=pathlib.Path, required=True)
    parser.add_argument("--runtime", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--external-runtime", action="store_true",
                        help="Keep all GNU/Steam payloads outside the HAP; only package adapters and notices")
    parser.add_argument("--wine-window-host", type=pathlib.Path,
                        help="Self-built Wine HWND adapter; never a Windows Steam client")
    args = parser.parse_args()
    if args.wine_window_host:
        data = args.wine_window_host.read_bytes()
        if not 1024 <= len(data) <= 524288 or data[:2] != b'MZ':
            parser.error("Invalid self-built Wine HWND adapter")
    if args.output.exists():
        parser.error("Output must be a new unsigned HAP.")
    manifest = json.loads((args.runtime / "manifest.json").read_text())
    names = {"libc.so.6", "ld-linux-aarch64.so.1", "libsteamohos_gnu_tls.so",
             "libsteamohos_gnu_late.so", "steamohos_gnu_fixture"}
    executables = {"steamohos_gnu_fixture"}
    if "waylandProbe" in manifest:
        names |= {"steamohos_gnu_wayland", "libwayland-client.so.0", "libffi.so.8",
                  "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6"}
        executables.add("steamohos_gnu_wayland")
    if "xwaylandProbe" in manifest:
        probe = manifest["xwaylandProbe"]
        extra = {"Xwayland", "steamohos_gnu_x11", "libsteamohos_xkbcomp.so",
                 "libX11.so.6", "libXau.so.6", "libXdmcp.so.6", "libXfont2.so.2",
                 "libbrotlicommon.so.1", "libbrotlidec.so.1", "libbsd.so.0", "libbz2.so.1.0",
                 "libfontenc.so.1", "libfreetype.so.6", "libmd.so.0", "libpixman-1.so.0",
                 "libpng16.so.16", "libxcb.so.1", "libxcvt.so.0", "libxkbfile.so.1",
                 "libxshmfence.so.1", "libz.so.1"}
        if probe.get("fullClient"):
            expected_executables |= {"steamohos_shell", "steamohos_taskset"}
            extra |= {"steamohos_shell", "steamohos_taskset"}
            declared = set(probe["files"])
            for name in declared - extra:
                if not re.fullmatch(r"(?:lib[A-Za-z0-9_.+\-]+\.so(?:\.[0-9]+)*|steamrtarm64/[A-Za-z0-9_./+\-]+)", name) or ".." in pathlib.PurePosixPath(name).parts:
                    parser.error("Unexpected full-client native path: " + name)
            extra = declared
            data = args.runtime / "steam-client-data.zip"
            if hashlib.sha256(data.read_bytes()).hexdigest() != probe["data"]["sha256"]:
                parser.error("Steam data archive mismatch")
        if set(probe["files"]) != extra or set(probe["executables"]) != {"Xwayland", "steamohos_gnu_x11"}:
            parser.error("Unexpected Xwayland diagnostic dependency set.")
        names |= extra
        executables |= set(probe["executables"])
        data = args.runtime / "xkb-data.zip"
        if hashlib.sha256(data.read_bytes()).hexdigest() != probe["data"]["sha256"]:
            parser.error("XKB data manifest mismatch.")
    if "steamClientProbe" in manifest:
        extra = {"steamrtarm64/steam", "ld-linux-aarch64.so.1", "libc.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
                 "libsteamohos_posix.so", "steamohos_gnu_xlib", "steamohos_gnu_sysvipc"}
        probe = manifest["steamClientProbe"]
        expected_executables = {"steamrtarm64/steam", "steamohos_gnu_xlib", "steamohos_gnu_sysvipc"}
        if "steamohos_gnu_graphics_loader" in probe["files"]:
            extra.add("steamohos_gnu_graphics_loader")
            expected_executables.add("steamohos_gnu_graphics_loader")
        if "steamohos_gnu_connection_identity" in probe["files"]:
            extra.add("steamohos_gnu_connection_identity")
            expected_executables.add("steamohos_gnu_connection_identity")
        if "compatibilitytools.d/hmosdeck/hmosdeck" in probe["files"]:
            extra.add("compatibilitytools.d/hmosdeck/hmosdeck")
            expected_executables.add("compatibilitytools.d/hmosdeck/hmosdeck")
        steamclient_helpers = {"steamohos_gnu_steamclient", "steamohos_gnu_steam_session"}
        if steamclient_helpers.intersection(probe["files"]):
            if not steamclient_helpers.issubset(probe["files"]):
                parser.error("Incomplete read-only Steam helper pair")
            extra |= steamclient_helpers
            expected_executables |= steamclient_helpers
        if "steamohos_gnu_steam_bridge" in probe["files"]:
            bridge = manifest.get("steamClientBridge", {})
            if bridge.get("gnuExecutable") != "steamohos_gnu_steam_bridge" or bridge.get("status") != "experimental_opt_in_not_accepted":
                parser.error("Unrecognized Steam interface bridge declaration")
            extra.add("steamohos_gnu_steam_bridge")
            expected_executables.add("steamohos_gnu_steam_bridge")
        if "steamohos_gnu_desktop" in probe["files"]:
            extra.add("steamohos_gnu_desktop")
            expected_executables.add("steamohos_gnu_desktop")
        if "steamohos_lsof" in probe["files"]:
            extra.add("steamohos_lsof")
            expected_executables.add("steamohos_lsof")
        if "mesaBuild" in manifest:
            mesa = manifest["mesaBuild"]
            if mesa.get("executionModel") != "native_aarch64" or any(
                manifest["files"].get(target) != mesa["files"].get(source) for source, target in [
                    ("libGL.so.1", "libsteamohos_software_gl.so"), ("steamohos_gnu_glx", "steamohos_gnu_glx")]):
                parser.error("GNU Mesa manifest mismatch")
            extra |= {"libsteamohos_software_gl.so", "steamohos_gnu_glx"}
            expected_executables.add("steamohos_gnu_glx")
        if probe.get("fullClient"):
            expected_executables |= {"steamohos_shell", "steamohos_taskset"}
            extra |= {"steamohos_shell", "steamohos_taskset"}
            declared = set(probe["files"])
            for name in declared - extra:
                if not re.fullmatch(r"(?:lib[A-Za-z0-9_.+\-]+\.so(?:\.[0-9]+)*|steamrtarm64/[A-Za-z0-9_./+\-]+)", name) or ".." in pathlib.PurePosixPath(name).parts:
                    parser.error("Unexpected full-client native path: " + name)
            extra = declared
            data = args.runtime / "steam-client-data.zip"
            if hashlib.sha256(data.read_bytes()).hexdigest() != probe["data"]["sha256"]:
                parser.error("Steam data archive mismatch")
        if set(probe["files"]) != extra or set(probe["executables"]) != expected_executables:
            parser.error("Unexpected Steam diagnostic manifest.")
        if not re.fullmatch(r"[a-f0-9]{64}", probe["steamSha256"]) or probe["steamSha256"] != manifest["files"]["steamrtarm64/steam"]["sha256"]:
            parser.error("Original Steam executable identity mismatch.")
        names |= extra
        executables |= set(probe["executables"])
        if probe.get("fullClient"):
            executables |= {name for name in extra if name.startswith("steamrtarm64/")}
    if set(manifest["files"]) != names:
        parser.error("Unexpected native diagnostic ELF manifest.")
    for name in names:
        data = (args.runtime / name).read_bytes()
        if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != 183 or hashlib.sha256(data).hexdigest() != manifest["files"][name]["sha256"]:
            parser.error("Runtime manifest mismatch: " + name)
    if manifest.get("steamClientProbe", {}).get("fullClient") and not args.external_runtime:
        parser.error("Full Steam clients must use --external-runtime; Valve payloads are not bundled in HAPs.")
    if args.external_runtime:
        with zipfile.ZipFile(args.hap) as source, zipfile.ZipFile(args.output, "w") as destination:
            if "libs/arm64-v8a/libsteamohos_gnu_child.so" not in source.namelist():
                parser.error("GNU diagnostics must be enabled in the input HAP")
            for item in source.infolist():
                if item.filename.startswith("libs/arm64-v8a/gnu-native/") or item.filename.startswith("resources/rawfile/linux-steam-native/"):
                    continue
                with source.open(item) as reader, destination.open(item, "w") as writer:
                    shutil.copyfileobj(reader, writer, length=4 * 1024 * 1024)
            destination.writestr("resources/rawfile/linux-steam-native/external-runtime.json", json.dumps({
                "schemaVersion": 1, "layout": "SteamLinuxARM64", "payloadBundled": False,
                "executionModel": "native_aarch64", "steamSha256": manifest.get("steamClientProbe", {}).get("steamSha256")
            }))
            if args.wine_window_host:
                destination.write(args.wine_window_host, "resources/rawfile/hmosdeck/hmosdeck-steam.exe",
                                  compress_type=zipfile.ZIP_DEFLATED)
            for name in ["COPYING.LIB", "LICENSES"]:
                if (args.runtime / name).is_file():
                    destination.write(args.runtime / name, "resources/rawfile/linux-steam-native/" + name,
                                      compress_type=zipfile.ZIP_DEFLATED)
        print(json.dumps({"unsignedHap": str(args.output), "externalRuntime": True, "bundledGnuFiles": 0,
                          "bundledSteamFiles": 0, "nativeFilesVerified": len(names)}))
        return
    with zipfile.ZipFile(args.hap) as hap:
        for name in names - executables:
            member = "libs/arm64-v8a/gnu-native/" + name
            if hashlib.sha256(hap.read(member)).hexdigest() != manifest["files"][name]["sha256"]:
                parser.error("Native library was stripped or replaced: " + name)
        if "libs/arm64-v8a/libsteamohos_gnu_child.so" not in hap.namelist():
            parser.error("GNU diagnostics must be enabled in the input HAP.")
    shutil.copyfile(args.hap, args.output)
    # Hvigor's native-library collector omits extensionless ELF executables.
    # Preserve the real name and bytes as a native-code entry before signing.
    # The official signer signs entries under libs/, including this file.
    with zipfile.ZipFile(args.output, "a") as hap:
        for name in sorted(executables):
            member = "libs/arm64-v8a/gnu-native/" + name
            if member in hap.namelist():
                if hashlib.sha256(hap.read(member)).hexdigest() != manifest["files"][name]["sha256"]:
                    parser.error("Collected executable identity mismatch: " + name)
                continue
            hap.write(args.runtime / name, member, compress_type=zipfile.ZIP_STORED)
        for name in ["COPYING.LIB", "LICENSES"]:
            notice = args.runtime / name
            if notice.is_file():
                hap.write(notice, "resources/rawfile/linux-steam-native/" + name, compress_type=zipfile.ZIP_DEFLATED)
        for directory in ["wayland-notices", "xwayland-notices"]:
            notices = args.runtime / directory
            if notices.is_dir():
                for notice in sorted(notices.iterdir()):
                    if notice.is_file():
                        hap.write(notice, "resources/rawfile/linux-steam-native/" + directory + "/" + notice.name,
                                  compress_type=zipfile.ZIP_DEFLATED)
        if manifest.get("steamClientProbe", {}).get("fullClient"):
            probe = manifest["steamClientProbe"]
            hap.write(args.runtime / "steam-client-data.zip", "resources/rawfile/linux-steam-native/steam-client-data.zip",
                      compress_type=zipfile.ZIP_STORED)
            hap.writestr("resources/rawfile/linux-steam-native/steam-client-data.version", probe["data"]["sha256"])
        if "xwaylandProbe" in manifest:
            hap.write(args.runtime / "xkb-data.zip", "resources/rawfile/linux-steam-native/xkb-data.zip",
                      compress_type=zipfile.ZIP_STORED)
    print(json.dumps({"unsignedHap": str(args.output), "nativeFilesVerified": len(names)}))


if __name__ == "__main__":
    main()
