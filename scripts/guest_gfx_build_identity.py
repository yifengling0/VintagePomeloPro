#!/usr/bin/env python3
"""Record Mesa's actual build options and verify opt-in Zink bundle contents."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("record", "verify-zink"))
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path)
    parser.add_argument("--arch", required=True)
    args = parser.parse_args()
    root = args.install_root
    manifest = root / "share/winehua/guest-gfx-build.json"
    if args.action == "record":
        if args.build_root is None:
            parser.error("record requires --build-root")
        options = json.loads((args.build_root / "meson-info/intro-buildoptions.json").read_text())
        options = {entry["name"]: entry["value"] for entry in options}
        libraries = sorted(path for path in (root / "lib").glob("libgallium*.so") if path.is_file())
        if not libraries:
            parser.error("no installed Gallium driver library")
        data = {
            "schemaVersion": 1,
            "arch": args.arch,
            "galliumDrivers": options["gallium-drivers"],
            "platforms": options["platforms"],
            "opengl": options["opengl"],
            "libraries": {str(path.relative_to(root)): digest(path) for path in libraries},
        }
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(data, indent=2) + "\n")
    else:
        if not manifest.is_file():
            parser.error("Zink requires guest-gfx-build.json from build_ohos_guest_gfx.sh; rebuild Mesa")
        data = json.loads(manifest.read_text())
        if data.get("schemaVersion") != 1 or data.get("arch") != args.arch:
            parser.error("Mesa build identity has a different schema or architecture")
        if "zink" not in data.get("galliumDrivers", []) or not data.get("opengl"):
            parser.error("Mesa was not built with the Zink OpenGL driver")
        libraries = data.get("libraries", {})
        if not libraries:
            parser.error("Mesa build identity has no Gallium library hashes")
        for name, expected in libraries.items():
            path = (root / name).resolve()
            if not path.is_relative_to(root.resolve()) or not path.is_file() or digest(path) != expected:
                parser.error("Mesa library differs from the recorded Zink build: " + name)
        print("Verified Zink build identity and Gallium library hashes")


if __name__ == "__main__":
    main()
