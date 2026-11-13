#!/usr/bin/env python3
"""Update a paired HAP's Wine audio library and runtime identity only.

Use an unsigned base and a freshly built wineohos.so. Every other member,
including Wine/FEX, application bytecode and libentry.so, is verified unchanged.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
from zipfile import ZipFile

from check_runtime_components import elf_needed
from runtime_library_guard import validate_library
from wine_audio_contract import shared_audio_format


MANIFEST = 'resources/rawfile/wine-runtime-manifest.json'
VERSION = 'resources/rawfile/wine_runtime.version'
PAYLOAD = 'resources/rawfile/wine-data.zip'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def repackage(base_path, library_path, output_path):
    if output_path.exists() or base_path.resolve() == output_path.resolve():
        raise ValueError('output must be a new file; existing packages are never overwritten')
    library = library_path.read_bytes()
    validate_library(library, 'wineohos.so')
    audio_format = shared_audio_format(library)
    with ZipFile(base_path) as base:
        names = base.namelist()
        if len(names) != len(set(names)) or any(n.startswith('META-INF/') for n in names):
            raise ValueError('use an unsigned base without duplicate ZIP members')
        candidates = [n for n in names if n.startswith('libs/') and n.endswith('/wineohos.so')]
        if len(candidates) != 1:
            raise ValueError('expected exactly one native Wine audio library')
        member = candidates[0]
        original = base.read(member)
        if original[:6] != library[:6] or original[18:20] != library[18:20]:
            raise ValueError('replacement audio ELF architecture differs from the paired runtime')
        if set(elf_needed(member, original)) != set(elf_needed(member, library)):
            raise ValueError('replacement audio dependencies differ from the paired runtime')
        manifest = json.loads(base.read(MANIFEST))
        if manifest['payloadSha256'] != sha(base.read(PAYLOAD)):
            raise ValueError('base runtime manifest does not match wine-data.zip')
        manifest['sharedAudio'] = {'library': member, 'sha256': sha(library), **audio_format}
        version = base.read(VERSION).decode().strip()
        version = re.sub(r';audio=[^;\s]*', '', version) + ';audio=' + sha(library)[:16] + '\n'
        updates = {member: library, MANIFEST: (json.dumps(manifest, indent=2) + '\n').encode(),
                   VERSION: version.encode()}
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(output_path, 'w', allowZip64=True) as result:
            for entry in base.infolist():
                result.writestr(copy.copy(entry), updates.get(entry.filename, base.read(entry)))
        with ZipFile(output_path) as result:
            if result.namelist() != names:
                raise ValueError('package inventory changed')
            changes = [name for name in names if sha(base.read(name)) != sha(result.read(name))]
            if set(changes) != set(updates):
                raise ValueError('unexpected changed package members: ' + repr(changes))
            validate_library(result.read(member), member)
        report = {'base': str(base_path), 'baseSHA256': sha(base_path.read_bytes()),
                  'output': str(output_path), 'outputSHA256': sha(output_path.read_bytes()),
                  'audioLibrary': str(library_path), 'previousAudioSHA256': sha(original),
                  'audioSHA256': sha(library), 'sharedAudioFormat': audio_format,
                  'changedMembers': changes, 'unchangedMembers': len(names) - len(changes)}
        output_path.with_suffix('.audio.json').write_text(json.dumps(report, indent=2) + '\n')
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(repackage(args.base, args.library, args.output), indent=2))


if __name__ == '__main__':
    main()
