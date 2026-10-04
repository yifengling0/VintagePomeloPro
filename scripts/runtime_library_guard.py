#!/usr/bin/env python3
"""Reject SDK link stubs in runtime inputs and final archives, without deleting inputs."""
import argparse
import io
from pathlib import Path
import struct
from zipfile import ZipFile


class RuntimeLibraryError(RuntimeError):
    pass


def exported_functions(data):
    if data[:4] != b'\x7fELF':
        return {}
    if len(data) < 64 or data[4] not in (1, 2) or data[5] not in (1, 2):
        raise RuntimeLibraryError('invalid ELF header')
    endian = '<' if data[5] == 1 else '>'
    wide = data[4] == 2
    offset = struct.unpack_from(endian + ('Q' if wide else 'I'), data, 40 if wide else 32)[0]
    stride, count = struct.unpack_from(endian + 'HH', data, 58 if wide else 46)
    fmt = endian + ('IIQQQQIIQQ' if wide else 'IIIIIIIIII')
    sections = [struct.unpack_from(fmt, data, offset + i * stride) for i in range(count)]
    functions = {}
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM, also present in stripped SDK stubs
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        if section[9] < (24 if wide else 16):
            raise RuntimeLibraryError('invalid dynamic symbol stride')
        for pos in range(section[4], section[4] + section[5], section[9]):
            symbol = struct.unpack_from(endian + ('IBBHQQ' if wide else 'IIIBBH'), data, pos)
            name, info, other, index, value = ((symbol[0], symbol[1], symbol[2], symbol[3], symbol[4])
                if wide else (symbol[0], symbol[3], symbol[4], symbol[5], symbol[1]))
            if index and info & 15 == 2 and info >> 4 in (1, 2) and other & 3 in (0, 3):
                end = names.find(b'\0', name)
                functions[names[name:end].decode('ascii', 'replace')] = value
    return functions


def validate_library(data, label):
    try:
        functions = exported_functions(data)
    except (IndexError, struct.error, ValueError) as error:
        raise RuntimeLibraryError(f'{label}: malformed ELF: {error}') from error
    # OHOS SDK link stubs alias their exported functions to one placeholder.
    if len(functions) >= 4 and len(set(functions.values())) == 1:
        raise RuntimeLibraryError(f'{label}: SDK-style link stub, not a runtime library')
    if Path(label).name in ('libz.so', 'libz.so.1'):
        required = ('inflate', 'inflateInit2_', 'inflateEnd', 'crc32', 'deflate')
        if any(name not in functions for name in required) or len({functions[n] for n in required}) < 4:
            raise RuntimeLibraryError(f'{label}: libz has no complete independent runtime implementation')


def validate_archive(data, label):
    with ZipFile(io.BytesIO(data)) as archive:
        for name in archive.namelist():
            if '.so' in Path(name).name:
                validate_library(archive.read(name), f'{label}/{name}')
            elif name.endswith('wine-data.zip'):
                validate_archive(archive.read(name), f'{label}/{name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        for path in args.paths:
            if not path.exists():
                continue
            for item in (path.rglob('*') if path.is_dir() else [path]):
                if not item.is_file():
                    continue
                if '.so' in item.name:
                    validate_library(item.read_bytes(), str(item))
                elif item.suffix in ('.hap', '.zip'):
                    validate_archive(item.read_bytes(), str(item))
    except (RuntimeLibraryError, OSError) as error:
        parser.exit(1, f'runtime provenance: {error}\n')


if __name__ == '__main__':
    main()
