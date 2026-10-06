#!/usr/bin/env python3
"""Static ABI checks for the private WineHua vtest extensions."""

import argparse
import pathlib
import re


def defines(path: pathlib.Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for name, value in re.findall(r"^#define\s+(VCMD_WINEHUA_[A-Z0-9_]+)\s+([^\s/]+)", path.read_text(), re.M):
        result[name] = int(value.rstrip("uU"), 0)
    return result


def check_layout(d: dict[str, int], prefix: str, fields: list[str], size: int) -> None:
    assert d[f"{prefix}_SIZE"] == size
    assert [d[f"{prefix}_{field}"] for field in fields] == list(range(size))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mesa-source", required=True, type=pathlib.Path)
    args = parser.parse_args()
    d = defines(args.mesa_source / "src/virtio/vtest/vtest_protocol.h")

    assert d["VCMD_WINEHUA_PRESENT"] == 0x57485052
    assert d["VCMD_WINEHUA_PRESENT_VERSION"] == 4
    check_layout(d, "VCMD_WINEHUA_PRESENT", [
        "PROTOCOL_VERSION", "FLAGS", "RES_HANDLE", "LEVEL", "LAYER",
        "FORMAT", "BIND", "WIDTH", "HEIGHT", "DRAWABLE_LO", "DRAWABLE_HI",
        "SERIAL", "SURFACE_ID", "CLIENT_PID",
    ], 14)
    check_layout(d, "VCMD_WINEHUA_PRESENT_REPLY", [
        "STATUS", "DEADLINE_LO", "DEADLINE_HI", "SERIAL",
    ], 4)

    assert d["VCMD_WINEHUA_VK_PRESENT"] == 0x57485650
    assert d["VCMD_WINEHUA_VK_PRESENT_VERSION"] == 1
    check_layout(d, "VCMD_WINEHUA_VK_PRESENT", [
        "PROTOCOL_VERSION", "FLAGS", "QUEUE_ID_LO", "QUEUE_ID_HI",
        "IMAGE_ID_LO", "IMAGE_ID_HI", "WIDTH", "HEIGHT", "FORMAT", "LAYOUT",
        "SERIAL", "SURFACE_ID", "CLIENT_PID",
    ], 13)
    check_layout(d, "VCMD_WINEHUA_VK_PRESENT_REPLY", [
        "STATUS", "DEADLINE_LO", "DEADLINE_HI", "SERIAL",
    ], 4)


if __name__ == "__main__":
    main()
