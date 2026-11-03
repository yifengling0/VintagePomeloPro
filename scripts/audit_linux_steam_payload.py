#!/usr/bin/env python3
"""Audit the actual Release HAP's Linux host, Wine bridge and GNU distribution."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import zipfile

def digest(data): return hashlib.sha256(data).hexdigest()

def audit(path):
    raw = 'resources/rawfile/'
    with zipfile.ZipFile(path) as hap:
        names = hap.namelist()
        if len(names) != len(set(names)):
            raise ValueError('HAP contains duplicate ZIP members')
        for required in ['libs/arm64-v8a/libsteamohos_gnu_child.so', 'libs/arm64-v8a/libsmoke_napi.so']:
            if required not in names: raise ValueError('Missing Linux native adapter: '+required)
        smoke = hap.read('libs/arm64-v8a/libsmoke_napi.so')
        for method in [b'startLinuxSteamSession', b'startLinuxSteamLaunchBroker']:
            if method not in smoke: raise ValueError('Linux NAPI support was compiled out')
        host = json.loads(hap.read(raw+'hmosdeck/window-host.json'))
        executable = hap.read(raw+'hmosdeck/hmosdeck-steam.exe')
        if len(executable) != host['bytes'] or digest(executable) != host['sha256']:
            raise ValueError('Wine HWND host stamp mismatch')
        wine_data = hap.read(raw+'wine-data.zip')
        wine = json.loads(hap.read(raw+'wine-runtime-manifest.json'))
        if digest(wine_data) != wine['payloadSha256']:
            raise ValueError('Wine runtime payload digest mismatch')
        with zipfile.ZipFile(io.BytesIO(wine_data)) as runtime:
            bridge = json.loads(runtime.read('linux-steam-bridge.json'))
            if bridge != wine.get('linuxSteamBridge'):
                raise ValueError('Wine bridge sidecar mismatch')
            for item in bridge['files']:
                data = runtime.read(item['path'])
                if len(data) != item['bytes'] or digest(data) != item['sha256']:
                    raise ValueError('Missing or stale paired Wine bridge: '+item['path'])
        plan = json.loads(hap.read(raw+'linux-steam/deployment.json'))
        supervisor = json.loads(hap.read(raw+'linux-steam/bootstrap/manifest.json'))
        update = hap.read(raw+'linux-steam/bootstrap/'+supervisor['name'])
        if len(update) != supervisor['bytes'] or digest(update) != supervisor['sha256'] or b'HMOSDECK_STEAM_RESTART ' not in update:
            raise ValueError('HAP Steam lifetime supervisor is missing or stale')
        if b'steamohos_gnu_steam_session_hap' not in hap.read('libs/arm64-v8a/libsteamohos_gnu_child.so'):
            raise ValueError('Native session still selects the old Steam lifetime supervisor')
        desktop = json.loads(hap.read(raw+'linux-steam/bootstrap/desktop-manifest.json'))
        update = hap.read(raw+'linux-steam/bootstrap/'+desktop['name'])
        if len(update) != desktop['bytes'] or digest(update) != desktop['sha256']:
            raise ValueError('HAP X11 session host is missing or stale')
        if b'steamohos_gnu_desktop_hap' not in hap.read('libs/arm64-v8a/libsteamohos_gnu_child.so'):
            raise ValueError('Native session still selects the old X11 session host')
        posix = json.loads(hap.read(raw+'linux-steam/bootstrap/posix-manifest.json'))
        support = hap.read(raw+'linux-steam/bootstrap/'+posix['name'])
        if len(support) != posix['bytes'] or digest(support) != posix['sha256'] or b'STEAMOHOS_GNU_WINE_PREFIX' not in support:
            raise ValueError('HAP GNU/OHOS Cloud adapter is missing or stale')
        if b'libsteamohos_posix_hap.so' not in hap.read('libs/arm64-v8a/libsteamohos_gnu_child.so'):
            raise ValueError('Native session still selects the old Cloud filesystem adapter')
        offline = json.loads(hap.read(raw+'linux-steam/offline-package.json'))
        if plan['id'] != offline['id']:
            raise ValueError('Offline import and online installation use different releases')
        gnu_archive = hap.read(raw+'linux-steam/hmosdeck-gnu-arm64-v1.zip')
        gnu = next(x for x in plan['archives'] if x['id']=='gnu-runtime')
        if len(gnu_archive) != gnu['bytes'] or digest(gnu_archive) != gnu['sha256']:
            raise ValueError('Bundled GNU archive differs from installation plan')
        with zipfile.ZipFile(io.BytesIO(gnu_archive)) as bundle:
            if any('/steamrtarm64/' in name for name in bundle.namelist()):
                raise ValueError('Official Steam binaries must remain outside HAP')
            helper = bundle.read('release/runtime/steamohos_gnu_steam_bridge')
            if digest(helper) != bridge['gnuSha256']:
                raise ValueError('GNU and Wine bridge payloads differ')
        return {'hap':str(path),'status':'passed','wineSha256':wine['payloadSha256'],
                'linuxRelease':plan['id'],'bridgeModules':len(bridge['files']),
                'windowsSteamRequired':False}

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hap',required=True,type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args(); result=audit(args.hap)
    if args.output: args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
