#!/usr/bin/env python3
"""Put the root HAP manifest before the configuration files HoKit also matches."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import tempfile
from zipfile import BadZipFile, ZipFile


CENTRAL_ENTRY = struct.Struct("<4s24xHHH12x")


def manifest_index(entries):
    names = [entry.orig_filename for entry in entries]
    root = "module.json" if "module.json" in names else "config.json"
    if names.count(root) != 1:
        raise ValueError("Expected exactly one root HAP manifest")
    return names.index(root)


def read_order(source):
    with ZipFile(source) as archive:
        entries = archive.infolist()
        manifest = manifest_index(entries)
        offset = archive.start_dir
        source.seek(max(0, offset - 32))
        footer = source.read(min(offset, 32)).lower()
        if any(magic in footer for magic in (b"hap sign block", b"hap sig block", b"app sign block")):
            raise ValueError("Use this command before signing a HAP")
        first_match = next(index for index, entry in enumerate(entries)
                           if entry.orig_filename.endswith(("module.json", "config.json")))
        if manifest == first_match:
            return offset, None

        source.seek(offset)
        records = []
        for _ in entries:
            header = source.read(CENTRAL_ENTRY.size)
            if len(header) != CENTRAL_ENTRY.size:
                raise BadZipFile("Truncated ZIP central directory")
            magic, name_length, extra_length, comment_length = CENTRAL_ENTRY.unpack(header)
            length = name_length + extra_length + comment_length
            data = source.read(length)
            if magic != b"PK\x01\x02" or len(data) != length:
                raise BadZipFile("Invalid central directory entry")
            records.append(header + data)
        records.insert(0, records.pop(manifest))
        return offset, b"".join(records)


def normalize_hap(path, *, check_only=False):
    path = path.resolve(strict=True)
    temporary = None
    try:
        with path.open("rb") as source:
            initial = os.fstat(source.fileno())
            offset, records = read_order(source)
            if records is None:
                return False
            if check_only:
                raise ValueError("A nested configuration precedes the root HAP manifest")

            # Move only index entries. ZIP64 offsets, file records and payloads stay intact.
            with tempfile.NamedTemporaryFile(prefix=f".{path.name}.", dir=path.parent,
                                             delete=False) as output:
                temporary = Path(output.name)
                source.seek(0)
                shutil.copyfileobj(source, output, length=4 * 1024 * 1024)
                output.seek(offset)
                output.write(records)
                output.flush()
                os.fsync(output.fileno())
        shutil.copymode(path, temporary)
        with temporary.open("rb") as candidate:
            if read_order(candidate)[1] is not None:
                raise ValueError("Normalized HAP still has the wrong manifest order")
        current = path.stat()
        if (not os.path.samestat(initial, current) or initial.st_size != current.st_size
                or initial.st_mtime_ns != current.st_mtime_ns):
            raise ValueError("The HAP changed during normalization")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hap", type=Path)
    parser.add_argument("--check", action="store_true", help="Check manifest order without writing")
    args = parser.parse_args()
    try:
        changed = normalize_hap(args.hap, check_only=args.check)
    except (BadZipFile, OSError, ValueError) as error:
        parser.exit(1, f"Unsigned HAP manifest order failed: {error}\n")
    status = "fixed" if changed else "verified"
    print(f"Unsigned HAP manifest order {status}: {args.hap}")


if __name__ == "__main__":
    main()
