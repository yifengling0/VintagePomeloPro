#!/usr/bin/env python3
"""Product boundary: OHOS Wine and Windows Steam, without retired Linux payloads."""
import argparse
import json
import shutil
import tempfile
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
RETIRED = ('linux-steam/', 'linux-proton/', 'linux-steam-native/')
REQUIRED_GUEST = (
    'bin/aarch64-windows/ntdll.dll', 'bin/i386-windows/ntdll.dll',
    'bin/aarch64-windows/kernelbase.dll', 'bin/aarch64-windows/libarm64ecfex.dll',
    'bin/wineboot.exe', 'bin/aarch64-windows/wined3d.dll',
)
REQUIRED_HOST = ('libentry.so', 'libwine_child.so', 'wineohos.so', 'libwineserver.so',
                 'libsteamdecompress.so', 'libgstreamer-1.0.so.0')
DEVICE_TEST_LIBRARIES = frozenset((
    'libsmoke_napi.so', 'libwinehua_guest_vulkan_smoke.so',
    'libvenus_sampled_image_probe.so', 'libvenus_spirv_replay.so',
    'libvenus_heaven_material_replay.so', 'libwinehua_host_heaven_replay.so',
))
DEVICE_TEST_PREFIXES = ('smoke/', 'bin/host_vulkan/', 'bin/guest_vulkan/bin/',
                        'bin/guest_vulkan/share/winehua/')
DEVICE_TEST_MARKERS = (b'runAutomationLaunch' , b'winehua.graphics_experiment',
                       b'setHostGraphicsExperimentForLab', b'PIXEL-BOUNDARY')

def verify_members(names):
    for name in names:
        relative = name.removeprefix('resources/rawfile/')
        if (relative.startswith(DEVICE_TEST_PREFIXES) or '/smoke/' in relative or
            Path(name).name in DEVICE_TEST_LIBRARIES):
            raise ValueError('Device test payload in product: ' + name)
        if (relative.startswith(RETIRED) or 'steamohos' in name.lower() or
            Path(name).name.lower() in ('lsteamclient.dll', 'linux-steam-bridge.json') or
            '/hmosdeck-steam/' in '/' + name.lower()):
            raise ValueError('Retired Linux Steam payload: ' + name)

def verify_source_layout(root=ROOT):
    raw = root / 'entry/src/main/resources/rawfile'
    names = []
    for directory in RETIRED:
        folder = raw / directory
        if folder.is_symlink():
            raise ValueError('Retired Linux payload symlink: ' + str(folder))
        if folder.exists():
            names.extend(p.relative_to(raw).as_posix() for p in folder.rglob('*') if p.is_file() or p.is_symlink())
    libs = root / 'entry/libs'
    if libs.exists(): names.extend(p.relative_to(libs).as_posix() for p in libs.rglob('*steamohos*'))
    verify_members(names)

def verify_build_profile(path):
    if 'WINEHUA_ENABLE_STEAMOHOS_' in path.read_text():
        raise ValueError('Retired Linux CMake options in ' + str(path))

def verify_hap(path):
    with ZipFile(path) as archive:
        names = archive.namelist()
        verify_members(names)
        for name in REQUIRED_HOST:
            if 'libs/arm64-v8a/' + name not in names:
                raise ValueError('Missing OHOS Wine / Windows Steam module: ' + name)
        for name in ('libentry.so',):
            member = 'libs/arm64-v8a/' + name
            if member in names:
                data = archive.read(member)
                for symbol in (b'probeLinuxSteam', b'startLinuxProtonGame', b'prepareLinuxSteamDisplay', b'isLinuxSteamProcess'):
                    if symbol in data: raise ValueError('Retired Linux NAPI in ' + member)
                for symbol in DEVICE_TEST_MARKERS:
                    if symbol in data: raise ValueError('Device test entry in ' + member)
        if 'ets/modules.abc' in names:
            code = archive.read('ets/modules.abc')
            if 'module.json' in names:
                module = json.loads(archive.read('module.json'))['module']
                source = module.get('srcEntry')
                if source:
                    entry = PurePosixPath(source).with_suffix('').as_posix()
                    record = f"L&{module['name']}/src/main/{entry}&;".encode()
                    if record not in code:
                        raise ValueError('ArkTS stage entry is missing from normalized bytecode: ' + source)
            for symbol in DEVICE_TEST_MARKERS + (b'/service/SmokeRunner', b'/smoke/SmokeHook', b'/game/GameHook'):
                if symbol in code: raise ValueError('Device test entry in ArkTS: ' + symbol.decode())
        # Keep memory bounded; do not expand an entire runtime tree for auditing.
        # TemporaryFile exposes seekable() on Python 3.10 as well as newer CI
        # Python versions, and keeps large nested runtimes out of memory.
        with tempfile.TemporaryFile() as payload:
            with archive.open('resources/rawfile/wine-data.zip') as source:
                shutil.copyfileobj(source, payload, length=1024 * 1024)
            payload.seek(0)
            with ZipFile(payload) as wine:
                guest = wine.namelist()
                verify_members(guest)
                for name in REQUIRED_GUEST:
                    if name not in guest or wine.getinfo(name).file_size == 0:
                        raise ValueError('Missing OHOS Wine guest component: ' + name)
                for arch in ('aarch64-windows', 'i386-windows'):
                    name = 'bin/' + arch + '/kernelbase.dll'
                    if name in guest and 'HMOSDECK_STEAM_BRIDGE'.encode('utf-16le') in wine.read(name):
                        raise ValueError('Retired Linux registry bridge in ' + name)
    return True

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-layout', action='store_true')
    parser.add_argument('--build-profile', type=Path)
    parser.add_argument('--hap', type=Path)
    args = parser.parse_args()
    if args.source_layout: verify_source_layout()
    if args.build_profile: verify_build_profile(args.build_profile)
    if args.hap: verify_hap(args.hap)
    print('OHOS Wine / Windows Steam product boundary PASS')

if __name__ == '__main__': main()
