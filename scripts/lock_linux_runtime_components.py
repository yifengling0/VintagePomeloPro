#!/usr/bin/env python3
"""Record verified GNU dependency bytes and matching adapter source inputs."""
import argparse
import json
from pathlib import Path
import subprocess

from prepare_linux_runtime_components import ROOT, DESTINATIONS, sha, expected_members, verify_archive


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release-tag',required=True)
    p.add_argument('--source-commit',required=True)
    a=p.parse_args()
    if not a.release_tag.startswith('build-deps-gnu-') or '/' in a.release_tag:
        p.error('Use a dedicated build-deps-gnu-* dependency release, not an app release')
    subprocess.run(['git','cat-file','-e',a.source_commit+'^{commit}'],cwd=ROOT,check=True)
    source_inputs={}
    for pattern in ['entry/src/main/cpp/linux_steam/gnu_fixture/*',
                    'entry/src/main/cpp/linux_steam/native_loader*',
                    'scripts/build_linux_*.py','scripts/prepare_linux_proton*.py',
                    'scripts/wine_audio_contract.py','patches/glibc/*.patch',
                    'patches/wine/0058*','patches/wine/0062*']:
        for path in ROOT.glob(pattern):
            if path.is_file(): source_inputs[path.relative_to(ROOT).as_posix()]=sha(path)
    gitlinks={name:subprocess.check_output(['git','rev-parse','HEAD:thirdparty/'+name],cwd=ROOT,text=True).strip()
              for name in ['mesa','dxvk-modern','wine-valve']}
    rows=[]
    for name in DESTINATIONS:
        raw=ROOT/'entry/src/main/resources/rawfile'
        archive=raw/name
        manifest=raw/DESTINATIONS[name]
        row=dict(path=name,bytes=archive.stat().st_size,sha256=sha(archive),manifestSha256=sha(manifest),
            url='https://github.com/yifengling0/VintagePomeloPro/releases/download/'+a.release_tag+'/'+archive.name)
        verify_archive(archive,row,expected_members(ROOT,row))
        rows.append(row)
    lock=dict(schemaVersion=1,sourceSnapshot=a.source_commit,releaseTag=a.release_tag,
        buildPolicy='Compile OHOS host/ArkTS from source; restore checksum-pinned prebuilt GNU dependencies. GNU source rebuild is a separate pipeline.',
        sourceInputs=dict(sorted(source_inputs.items())),gitlinks=gitlinks,components=rows)
    output=ROOT/'docs/linux-steam/build-components.lock.json'
    output.write_text(json.dumps(lock,indent=2)+'\n')
    print('Locked',len(rows),'verified archives and',len(source_inputs),'source inputs; no payload copied')


if __name__=='__main__': main()
