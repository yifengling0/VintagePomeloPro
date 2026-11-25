#!/usr/bin/env python3
"""Verify that a signed HAP contains the current native Wine candidate."""

import argparse
import hashlib
import json
import struct
from pathlib import Path
from zipfile import ZipFile


class ValidationError(RuntimeError):
    pass


NATIVE_ARTIFACTS = (
    ("wineohos.so", "wine", "dlls/wineohos.drv/wineohos.so"),
    ("win32u.so", "wine", "dlls/win32u/win32u.so"),
    ("winewayland.so", "wine", "dlls/winewayland.drv/winewayland.so"),
    ("ntdll.so", "wine", "dlls/ntdll/ntdll.so"),
)
WINE_SERVER_ARTIFACT = ("libwineserver.so", "build", "wine_server-aarch64/libwineserver.so")
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
PT_LOAD = 1


def digest_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def digest_member(archive: ZipFile, name: str) -> str:
    try:
        source = archive.open(name)
    except KeyError as exc:
        raise ValidationError(f"HAP is missing {name}") from exc

    digest = hashlib.sha256()
    with source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_ui_payload(candidate: ZipFile, compiled: ZipFile) -> None:
    """Keep compiled ABC, resource IDs, module metadata and UI assets together.

    A native-runtime overlay may retain rawfile from a verified baseline, but
    must never combine a new ABC with another compilation's resource index.
    """
    required = {"ets/modules.abc", "resources.index", "module.json"}

    def ui_names(archive: ZipFile) -> set[str]:
        names = [entry.filename for entry in archive.infolist()
                 if not entry.is_dir() and (entry.filename in required or
                     entry.filename.startswith("resources/rawfile/ui/") or
                     (entry.filename.startswith("resources/") and
                      not entry.filename.startswith("resources/rawfile/")))]
        if len(names) != len(set(names)):
            raise ValidationError("HAP has duplicate compiled UI members")
        missing = required - set(names)
        if missing:
            raise ValidationError(f"HAP is missing compiled UI members: {sorted(missing)}")
        return set(names)

    expected = ui_names(compiled)
    actual = ui_names(candidate)
    if actual != expected:
        raise ValidationError(
            f"compiled UI asset inventory mismatch: missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )
    for name in sorted(expected):
        if digest_member(candidate, name) != digest_member(compiled, name):
            raise ValidationError(f"HAP UI member differs from the same compilation: {name}")

    module = json.loads(candidate.read("module.json"))["module"]
    stage = module.get("srcEntry")
    if stage and stage.startswith("./") and stage.endswith(".ets"):
        record = f'L&{module["name"]}/src/main/{stage[2:-4]}&;'.encode("utf-8")
        if record not in candidate.read("ets/modules.abc"):
            raise ValidationError(f"Compiled ability stage {stage} requires useNormalizedOHMUrl")


def load_signature(label: str, data: bytes) -> tuple[object, ...]:
    if len(data) < ELF_HEADER.size:
        raise ValidationError(f"{label} is too short to be an ELF file")

    header = ELF_HEADER.unpack_from(data)
    ident = header[0]
    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
        raise ValidationError(f"{label} is not a little-endian ELF64 file")

    (_, elf_type, machine, version, entry, program_offset, _section_offset,
     flags, header_size, program_size, program_count, _section_size,
     _section_count, _section_string_index) = header
    if header_size != ELF_HEADER.size or program_size != PROGRAM_HEADER.size:
        raise ValidationError(f"{label} has an unsupported ELF program header layout")
    if program_offset + program_size * program_count > len(data):
        raise ValidationError(f"{label} has truncated ELF program headers")

    loads = []
    for index in range(program_count):
        offset = program_offset + index * program_size
        (segment_type, segment_flags, file_offset, virtual_address,
         physical_address, file_size, memory_size, alignment) = PROGRAM_HEADER.unpack_from(data, offset)
        if segment_type != PT_LOAD:
            continue
        if file_offset + file_size > len(data):
            raise ValidationError(f"{label} has a truncated PT_LOAD segment")

        segment = bytearray(data[file_offset:file_offset + file_size])
        if file_offset == 0 and len(segment) >= ELF_HEADER.size:
            # Release packaging removes section tables and changes only these
            # non-runtime ELF header fields. Program headers remain covered.
            segment[40:48] = b"\0" * 8
            segment[58:64] = b"\0" * 6
        loads.append((file_offset, virtual_address, physical_address, file_size,
                      memory_size, segment_flags, alignment,
                      hashlib.sha256(segment).hexdigest()))

    if not loads:
        raise ValidationError(f"{label} has no PT_LOAD segments")
    return elf_type, machine, version, entry, flags, tuple(loads)


def find_payload(archive: ZipFile) -> str:
    candidates = [name for name in archive.namelist()
                  if name.endswith("resources/rawfile/wine-data.zip")]
    if len(candidates) != 1:
        raise ValidationError(f"expected one HAP wine-data payload, found {len(candidates)}")
    return candidates[0]


def verify(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    hap = args.hap.resolve()
    entry_libs = root / "entry" / "libs" / args.native_arch
    payload = root / "entry" / "src" / "main" / "resources" / "rawfile" / "wine-data.zip"
    wine_build = args.wine_build.resolve()

    if not hap.is_file():
        raise ValidationError(f"signed HAP not found: {hap}")
    if not payload.is_file():
        raise ValidationError(f"rawfile payload not found: {payload}")

    artifacts = list(NATIVE_ARTIFACTS)
    artifacts.append(WINE_SERVER_ARTIFACT)
    with ZipFile(hap) as archive:
        if args.compiled_ui_hap is not None:
            with ZipFile(args.compiled_ui_hap) as compiled:
                verify_ui_payload(archive, compiled)
        payload_name = find_payload(archive)
        expected_payload = digest_path(payload)
        actual_payload = digest_member(archive, payload_name)
        if actual_payload != expected_payload:
            raise ValidationError(
                f"HAP wine-data payload hash {actual_payload} differs from rawfile {expected_payload}"
            )

        for hap_name, source_root, build_relative in artifacts:
            source = (wine_build if source_root == "wine" else wine_build.parent) / build_relative
            staged = entry_libs / hap_name
            if not source.is_file():
                raise ValidationError(f"candidate build artifact not found: {source}")
            if not staged.is_file():
                raise ValidationError(f"entry native library not found: {staged}")
            if digest_path(source) != digest_path(staged):
                raise ValidationError(f"entry native library does not match candidate build: {hap_name}")

            member_name = f"libs/{args.native_arch}/{hap_name}"
            try:
                packaged = archive.read(member_name)
            except KeyError as exc:
                raise ValidationError(f"HAP is missing {member_name}") from exc
            if load_signature(str(staged), staged.read_bytes()) != load_signature(member_name, packaged):
                raise ValidationError(
                    f"HAP runtime image does not match candidate build after release stripping: {hap_name}"
                )

    print(f"candidate HAP binary mapping PASS: libraries={len(artifacts)} payload={expected_payload}")


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hap", type=Path, required=True, help="signed HAP to validate")
    parser.add_argument("--root", type=Path, default=root, help="Proton-OHOS worktree root")
    parser.add_argument("--compiled-ui-hap", type=Path,
                        help="HAP from the same UI compilation; validate ABC, index, module and UI assets")
    parser.add_argument("--ui-only", action="store_true",
                        help="validate compiled UI without requiring a native Wine build")
    parser.add_argument("--native-arch", default="arm64-v8a", help="HAP native ABI")
    parser.add_argument("--wine-build", type=Path,
                        help="Wine build directory (default: <root>/build/wine-ohos-aarch64)")
    args = parser.parse_args()
    if args.ui_only and args.compiled_ui_hap is None:
        parser.error("--ui-only requires --compiled-ui-hap")
    if args.wine_build is None:
        args.wine_build = args.root / "build" / "wine-ohos-aarch64"

    try:
        if args.ui_only:
            with ZipFile(args.hap) as candidate, ZipFile(args.compiled_ui_hap) as compiled:
                verify_ui_payload(candidate, compiled)
            print("candidate HAP UI payload PASS")
            return
        verify(args)
    except (OSError, ValueError, ValidationError) as exc:
        raise SystemExit(f"candidate HAP verification failed: {exc}") from exc


if __name__ == "__main__":
    main()
