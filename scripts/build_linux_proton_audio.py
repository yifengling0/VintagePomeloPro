#!/usr/bin/env python3
"""Build the private GNU ALSA bridge using the existing AudioBroker protocol."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', type=Path, required=True)
    p.add_argument('--base-build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[1]
    cc = a.cc.resolve(strict=True)
    base = a.base_build.resolve(strict=True)
    output = a.output.resolve()
    if output.is_relative_to(repo) or output.is_relative_to(base):
        p.error('Use an isolated reusable task directory')
    if subprocess.check_output([str(cc), '-dumpmachine'], text=True).strip() != 'aarch64-none-linux-gnu':
        p.error('GNU AArch64 compiler required')
    output.mkdir(parents=True, exist_ok=True)
    source = repo/'entry/src/main/cpp/linux_steam/gnu_fixture/alsa_hmosdeck.c'
    paths = [source, repo/'entry/src/main/cpp/protocols/audio_ipc_protocol.h', cc,
             base/'runtime/libasound.so.2', base/'alsa-include/alsa/pcm_ioplug.h', Path(__file__)]
    inputs = {str(path): sha(path) for path in paths}
    library = output/'libasound_module_pcm_proton.so'
    identity = output/'proton-audio-build.json'
    if identity.exists() and library.exists():
        previous = json.loads(identity.read_text())
        if previous.get('inputs') == inputs and previous.get('sha256') == sha(library):
            print('Reused verified private ALSA bridge')
            return
    subprocess.run([str(cc), '-O2', '-Wall', '-Wextra', '-Werror', '-DPIC', '-shared', '-fPIC',
        '-I'+str(base/'alsa-include'), str(source), '-L'+str(base/'runtime'), '-l:libasound.so.2',
        '-Wl,-z,relro,-z,now', '-o', str(library)], check=True)
    identity.write_text(json.dumps({'inputs': inputs, 'bytes': library.stat().st_size,
        'sha256': sha(library), 'scope': 'Private GNU Proton ALSA to OHOS AudioBroker'}, indent=2)+'\n')
    print('Built private ALSA bridge', library.stat().st_size, sha(library))


if __name__ == '__main__':
    main()
