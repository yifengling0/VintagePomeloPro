#!/usr/bin/env python3
"""Replace a paired ARM64 Wine ntdll and record its thread-exit identity."""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import re
from zipfile import ZipFile

from check_runtime_components import elf_needed
from runtime_library_guard import THREAD_EXIT_REPAIR_VERSION, validate_archive, validate_thread_exit_library

MANIFEST = 'resources/rawfile/wine-runtime-manifest.json'
VERSION = 'resources/rawfile/wine_runtime.version'
PAYLOAD = 'resources/rawfile/wine-data.zip'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def repackage(base_path, library_path, output_path):
    if output_path.exists() or base_path.resolve() == output_path.resolve():
        raise ValueError('output must be a new file')
    library = library_path.read_bytes()
    validate_thread_exit_library(library, str(library_path))
    with ZipFile(base_path) as base:
        names = base.namelist()
        if len(names) != len(set(names)) or any(n.startswith('META-INF/') for n in names):
            raise ValueError('expected an unsigned base without duplicate members')
        members = [n for n in names if n.startswith('libs/') and n.endswith('/ntdll.so')]
        if len(members) != 1:
            raise ValueError('expected exactly one native ntdll')
        member = members[0]
        old = base.read(member)
        if old[:6] != library[:6] or old[18:20] != library[18:20]:
            raise ValueError('native architecture differs from the paired runtime')
        if set(elf_needed(member, old)) != set(elf_needed(member, library)):
            raise ValueError('native dependencies differ from the paired runtime')
        manifest = json.loads(base.read(MANIFEST))
        payload = base.read(PAYLOAD)
        if manifest['payloadSha256'] != sha(payload):
            raise ValueError('base payload identity does not match')
        updates = {member: library}
        nested = []
        with ZipFile(io.BytesIO(payload)) as archive:
            nested = [n for n in archive.namelist() if Path(n).name == 'ntdll.so'
                      and archive.read(n)[:6] == library[:6]
                      and archive.read(n)[18:20] == library[18:20]]
            if nested:
                buffer = io.BytesIO()
                with ZipFile(buffer, 'w', allowZip64=True) as result:
                    for entry in archive.infolist():
                        result.writestr(copy.copy(entry), library if entry.filename in nested
                                        else archive.read(entry.filename))
                updates[PAYLOAD] = buffer.getvalue()
                manifest['payloadSha256'] = sha(updates[PAYLOAD])
        manifest['threadExit'] = {'library': member, 'sha256': sha(library),
                                  'repairVersion': THREAD_EXIT_REPAIR_VERSION, 'nestedMembers': nested}
        version = base.read(VERSION).decode().strip()
        version = re.sub(r';thread-exit=[^;\s]*', '', version)
        updates[VERSION] = (version + ';thread-exit=' + sha(library)[:16] + '\n').encode()
        updates[MANIFEST] = (json.dumps(manifest, indent=2) + '\n').encode()
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(output_path, 'w', allowZip64=True) as result:
            for entry in base.infolist():
                result.writestr(copy.copy(entry), updates.get(entry.filename, base.read(entry.filename)))
        with ZipFile(output_path) as result:
            assert result.namelist() == names
            changed = [name for name in names if sha(base.read(name)) != sha(result.read(name))]
            assert set(changed) == set(updates), changed
            assert result.read(member) == library
        validate_archive(output_path.read_bytes(), str(output_path))
        report = {'baseSHA256': sha(base_path.read_bytes()), 'outputSHA256': sha(output_path.read_bytes()),
                  'ntdllSHA256': sha(library), 'previousNtdllSHA256': sha(old),
                  'changedMembers': changed, 'nestedMembers': nested,
                  'unchangedMembers': len(names) - len(changed)}
        output_path.with_suffix('.thread-exit.json').write_text(json.dumps(report, indent=2) + '\n')
        return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(repackage(args.base, args.library, args.output), indent=2))
