#!/usr/bin/env python3
"""Record Mesa build inputs and verify the installed or packaged libraries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def mesa_identity(source=None):
    lock = json.loads((ROOT / 'docs/graphics/graphics-stack.lock.yaml').read_text())
    recipe = ROOT / 'scripts/apply_mesa_ohos_patches.sh'
    overlays = re.findall(r'\$SCRIPT_DIR/\.\./(patches/mesa/[^"\s]+\.patch)', recipe.read_text())
    if overlays != lock['mesa_production_overlays']:
        raise ValueError('Mesa production overlay order differs from graphics lock')
    expected = {row['path']: row['sha256'] for row in lock['managed_patches']['mesa']}
    rows = [{'path': name, 'sha256': digest(ROOT / name)} for name in overlays]
    if any(row['sha256'] != expected[row['path']] for row in rows):
        raise ValueError('Mesa production overlay content differs from graphics lock')
    files = lock['effective_files']['mesa']
    if source is not None and any(not (source / name).is_file() or digest(source / name) != value
                                  for name, value in files.items()):
        raise ValueError('Mesa source differs from the locked effective files')
    identity = {'pin': lock['submodules']['mesa'], 'overlays': rows, 'effectiveFiles': files}
    if os.environ.get('WINEHUA_BUILD_GPU_BC_UPLOAD') == '1':
        overlay = 'patches/experimental/gpu-bc-upload/mesa.patch'
        if source is not None:
            subprocess.run(['git', '-c', f'safe.directory={source}', '-C', str(source),
                            'apply', '--reverse', '--check', str(ROOT / overlay)], check=True)
        identity['gpuBcUpload'] = {'path': overlay, 'sha256': digest(ROOT / overlay)}
    return identity


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "verify-zink", "verify-bundle"))
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--arch", required=True)
    args = parser.parse_args()
    root = args.install_root
    bundle = args.action == "verify-bundle"
    manifest = root / ("guest-gfx-build.json" if bundle else "share/winehua/guest-gfx-build.json")
    if args.action == "record":
        if args.build_root is None or args.source_root is None:
            parser.error("record requires --build-root and --source-root")
        options = json.loads((args.build_root / "meson-info/intro-buildoptions.json").read_text())
        options = {entry["name"]: entry["value"] for entry in options}
        libraries = sorted({path for pattern in ("libgallium*.so", "libEGL.so*", "libGLES*.so*")
                            for path in (root / "lib").glob(pattern) if path.is_file()})
        if not any(path.name.startswith('libgallium') for path in libraries):
            parser.error("no installed Gallium driver library")
        data = {
            "schemaVersion": 3,
            "arch": args.arch,
            "galliumDrivers": options["gallium-drivers"],
            "platforms": options["platforms"],
            "opengl": options["opengl"],
            "libraries": {str(path.relative_to(root)): digest(path) for path in libraries},
            "mesaSource": mesa_identity(args.source_root),
        }
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(data, indent=2) + "\n")
    else:
        if not manifest.is_file():
            parser.error("Missing guest-gfx-build.json from build_ohos_guest_gfx.sh; rebuild Mesa")
        data = json.loads(manifest.read_text())
        if data.get("schemaVersion") != 3 or data.get("arch") != args.arch:
            parser.error("Mesa build identity has a different schema or architecture")
        if data.get('mesaSource') != mesa_identity():
            parser.error('Mesa build identity has stale sources or overlays; rebuild the graphics bundle')
        drivers = set(data.get("galliumDrivers", []))
        required_drivers = {"zink"}
        if bundle:
            env = (root / "winehua-guest-gfx.env").read_text().splitlines()
            required_drivers = {"virgl"}
            if args.arch == "aarch64" or "GALLIUM_DRIVER=zink" in env:
                required_drivers.add("zink")
        if not required_drivers <= drivers or not data.get("opengl"):
            parser.error("Mesa requires OpenGL drivers: " + ", ".join(sorted(required_drivers)))
        libraries = data.get("libraries", {})
        if not libraries:
            parser.error("Mesa build identity has no Gallium library hashes")
        for family in ('libgallium', 'libEGL.so', 'libGLESv2.so'):
            if not any(Path(name).name.startswith(family) for name in libraries):
                parser.error("Incomplete Mesa EGL/Gallium build identity; rebuild the complete graphics bundle")
        for name, expected in libraries.items():
            path = (root / name).resolve()
            if not path.is_relative_to(root.resolve()) or not path.is_file() or digest(path) != expected:
                parser.error("Mesa library differs from the recorded build: " + name)
        if bundle:
            gallium = {checksum for name, checksum in libraries.items() if Path(name).name.startswith("libgallium")}
            aliases = ["virtio_gpu_dri.so", "swrast_dri.so", "kms_swrast_dri.so"]
            if "zink" in required_drivers:
                aliases.append("zink_dri.so")
            for name in aliases:
                path = root / "lib/dri" / name
                if not path.is_file() or digest(path) not in gallium:
                    parser.error("Mesa driver alias differs from the recorded Gallium build: " + name)
        print("Verified Mesa build identity and library hashes")


if __name__ == "__main__":
    main()
