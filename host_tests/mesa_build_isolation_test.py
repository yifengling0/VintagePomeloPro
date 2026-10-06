#!/usr/bin/env python3
"""Guard the migration-only Mesa entry points against shared build roots."""

import argparse
import os
import pathlib
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    makefile = (args.root / "Makefile").read_text()
    gfx = (args.root / "scripts/build_ohos_guest_gfx.sh").read_text()
    vulkan = (args.root / "scripts/build_ohos_guest_vulkan.sh").read_text()

    for target in ("mesa-guest-gfx:", "mesa-guest-vulkan:"):
        assert target in makefile
    for variable in ("MESA_SOURCE_ROOT", "MESA_BUILD_ROOT", "MESA_INSTALL_ROOT"):
        assert variable in makefile
    assert "--platform wayland --mode virpipe --no-package" in makefile
    assert '"-Dplatforms=wayland"' in gfx
    assert "egl-native-platform=wayland" not in gfx
    assert 'local required_version="1.41"' in gfx
    assert ".winehua-mesa-build-identity" in gfx
    assert "Mesa source, build, and install roots must be distinct" in gfx
    for option in ("--source-root", "--build-root", "--install-root"):
        assert option in vulkan

    env = os.environ.copy()
    env["LLVM_MINGW"] = "/definitely/missing/llvm-mingw"
    for script in ("build_ohos_guest_gfx.sh", "build_ohos_guest_vulkan.sh"):
        result = subprocess.run(
            ["bash", str(args.root / "scripts" / script), "--help"],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        assert "llvm-mingw" not in result.stderr.lower()


if __name__ == "__main__":
    main()
