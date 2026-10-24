#!/usr/bin/env python3
"""Import checksum-pinned Windows compatibility modules shipped with Linux Steam.

These are external data. Authentication stays in the Linux client through
lsteamclient; no Windows Steam executable, account, game or configuration is copied.
"""
import argparse, hashlib, json, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def import_compat(package,runtime,lock=None):
    lock=lock or json.loads((ROOT/'entry/src/main/cpp/linux_steam/client_compat.lock.json').read_text())
    if hashlib.sha1(package.read_bytes()).hexdigest()!=lock['packageSha1']:
        raise ValueError('Official Linux compatibility package digest mismatch')
    expected={'steamclient.dll':('steamclient.dll','i386'), 'steamclient64.dll':('steamclient64.dll','x86_64'), 'Steam.dll':('legacycompat/Steam.dll','i386')}
    if set(lock['files'])!=set(expected):raise ValueError('Unexpected compatibility module allowlist')
    contents={}
    with zipfile.ZipFile(package) as source:
        for name,(origin,arch) in expected.items():
            record=lock['files'][name]
            if (record['source'],record['architecture'])!=(origin,arch):raise ValueError('Unexpected module source or ABI')
            entries=[x for x in source.infolist() if x.filename==origin]
            if len(entries)!=1 or entries[0].file_size!=record['bytes'] or record['bytes']>33554432:
                raise ValueError('Missing, duplicate or oversized module')
            data=source.read(entries[0]);pe=int.from_bytes(data[60:64],'little') if data[:2]==b'MZ' else 0
            machine=b'\x4c\x01' if arch=='i386' else b'\x64\x86'
            if pe<64 or data[pe:pe+6]!=b'PE\0\0'+machine:raise ValueError('Wrong module architecture')
            if hashlib.sha256(data).hexdigest()!=record['sha256']:raise ValueError('Module checksum mismatch')
            contents['steamohos/compat/'+name]=data
    archive=runtime/'steam-client-compat.zip'
    with zipfile.ZipFile(archive,'w') as target:
        for name,data in sorted(contents.items()):
            member=zipfile.ZipInfo(name,(1980,1,1,0,0,0));member.compress_type=zipfile.ZIP_DEFLATED;member.external_attr=0o100644<<16
            target.writestr(member,data)
    manifest_path=runtime/'manifest.json';manifest=json.loads(manifest_path.read_text())
    manifest['steamClientProbe']['compatibility']={'file':archive.name,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'provenance':lock}
    manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
    return {'file':archive.name,'modules':list(expected),'bytes':archive.stat().st_size,'windowsSteamClientRequired':False}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--package',type=Path,required=True);p.add_argument('--runtime',type=Path,required=True);a=p.parse_args()
    print(json.dumps(import_compat(a.package,a.runtime)))
