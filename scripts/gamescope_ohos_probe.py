#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile actual Gamescope units with the OHOS SDK; inventory native gaps.

This probe never launches a compositor or qualifies a graphics backend. Run
through make in winehua-dev with an existing host dependency prefix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, env=None):
    try:
        result = subprocess.run(list(map(str, command)), env=env, capture_output=True,
                                text=True, timeout=60)
        return {'returncode': result.returncode,
                'stdout': result.stdout[-16384:], 'stderr': result.stderr[-16384:]}
    except subprocess.TimeoutExpired:
        return {'returncode': 124, 'stderr': '60 second probe deadline exceeded'}


def probe(sdk, prefix):
    work = ROOT/'build/gamescope/arm64-v8a'
    source = work/'source'
    output = work/'native-probe'
    output.mkdir(exist_ok=True)
    clang = sdk/'llvm/bin/clang++'
    sysroot = sdk/'sysroot'
    if not clang.is_file() or not (source/'src/rendervulkan.cpp').is_file():
        raise ValueError('Prepare the locked source and provide the OHOS native SDK first')
    env = dict(os.environ)
    env.pop('PKG_CONFIG_PATH', None)
    env['PKG_CONFIG_LIBDIR'] = ':'.join(map(str, [prefix/'lib/pkgconfig', prefix/'share/pkgconfig']))
    packages = {}
    for name in ['wayland-server', 'wayland-client', 'wlroots-0.20', 'pixman-1', 'xkbcommon',
                 'libdrm', 'x11', 'xdamage', 'xcomposite', 'xcursor', 'xrender', 'xext',
                 'xfixes', 'xxf86vm', 'xtst', 'xres', 'xmu', 'xi', 'libdisplay-info',
                 'libdecor-0', 'luajit', 'libinput', 'libudev']:
        value = run(['pkg-config', '--modversion', name], env)
        packages[name] = {'available': value['returncode'] == 0,
                          'version': value.get('stdout', '').strip(),
                          'diagnostic': value.get('stderr', '').strip()}
    flags = [str(clang), '--target=aarch64-linux-ohos', '--sysroot='+str(sysroot),
             '-std=c++20', '-fPIC', '-D_GNU_SOURCE', '-DHAVE_LIBCAP=0',
             '-DGLM_ENABLE_EXPERIMENTAL', '-I'+str(source/'src'),
             '-I'+str(source/'subprojects/glm')]
    compilations = []
    for path in ['src/color_helpers.cpp', 'src/Utils/Process.cpp']:
        obj = output/(Path(path).stem+'.o')
        command = [*flags, '-c', str(source/path), '-o', str(obj)]
        result = run(command)
        record = {'source': path, 'sourceSha256': hashlib.sha256((source/path).read_bytes()).hexdigest(),
                  'command': command, **result}
        if result['returncode'] == 0:
            record['elfHeader'] = run(['readelf', '-h', obj]).get('stdout', '')
        compilations.append(record)
    report = {'schemaVersion': 1, 'platform': 'aarch64-linux-ohos', 'runtimeQualified': False,
              'dependencyCheckScope': 'pkg-config metadata presence; not linked-library closure',
              'compiler': run([clang, '--version'])['stdout'].splitlines()[0],
              'hostPrefix': str(prefix), 'dependencies': packages, 'compilations': compilations,
              'requiredPlatformWork': [
                  'OHOS backend replaces the GNU Xlib WSI output; preserve GPU queue and swapchain ownership.',
                  'Gamescope/Xwayland process spawning must use NativeChildProcess instead of fork/exec.',
                  'Connect VPP native shared images and fences; GNU private Control transport is not an OHOS resource handle.',
                  'Disable unavailable DRM/VR global-input entry points explicitly; never report unsupported features as supported.'
              ]}
    report_path = work/'native-probe.json'
    report_path.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'report': str(report_path),
                      'compiledUnits': [r['source'] for r in compilations if r['returncode'] == 0],
                      'failedUnits': [r['source'] for r in compilations if r['returncode'] != 0],
                      'missingPackages': [k for k, v in packages.items() if not v['available']],
                      'runtimeQualified': False}))
    return all(result['returncode'] == 0 for result in compilations)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--host-prefix', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(0 if probe(args.sdk.resolve(), args.host_prefix.resolve()) else 1)
