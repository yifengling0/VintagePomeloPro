"""Build a local candidate overlay without modifying the original Steam folder.

The frozen Windows Steam executable is PE32. Both client DLLs on disk remain
original; only SteamUI loads the runtime decoder extension. Game architecture
does not select this extension; the downloading process must be verified.
This is not an extension for an unrelated PE64 downloader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from audit_install import ORIGINAL, V3, RUNTIME32
from patch_startup import patch_startup


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(original, helpers, output):
    if output.exists():
        raise ValueError('Output must be a new directory')
    for name, expected in ORIGINAL.items():
        if digest(original/name) != expected:
            raise ValueError('Unsupported original: '+name)
    if digest(original/'steam.exe') != '7d265e5e1d5b02c8bffe51db73aaeac9f86abfe505f0dfc86cd87700802732d3':
        raise ValueError('Unsupported Steam executable')
    if digest(helpers/'vpsteamzstd32.dll') != V3['vpsteamzstd32.dll']:
        raise ValueError('Runtime32 requires the exact release decoder')
    helper_sha = digest(helpers/'vpsteamtrust32.dll')
    if helper_sha != RUNTIME_HELPER_SHA256:
        raise ValueError('Revalidate a changed runtime helper before packaging')
    patched, _ = patch_startup((original/'SteamUI.dll').read_bytes())
    output.mkdir(parents=True)
    for name in ('steamclient.dll', 'steamclient64.dll'):
        shutil.copyfile(original/name, output/name)
    (output/'SteamUI.dll').write_bytes(patched)
    for name in ('vpsteamtrust32.dll', 'vpsteamzstd32.dll'):
        shutil.copyfile(helpers/name, output/name)
    shutil.copyfile(Path(__file__).with_name('ZSTD-LICENSE.txt'), output/'VPP-ZSTD-LICENSE.txt')
    names = ['steamclient.dll','steamclient64.dll','SteamUI.dll','vpsteamtrust32.dll','vpsteamzstd32.dll']
    manifest = dict(kind='runtime32-original-client-candidate',
                    sha256={name:digest(output/name) for name in names},
                    fullDownloadVerified=True, longTermStabilityVerified=False,
                    verifiedDownloads=[dict(appid=1713610, depot=1713611,
                        build=22144708, bytesDownloaded=126782064,
                        vszaBlocks=56, result='No Error')])
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n', encoding='utf-8')
    (output/'README.txt').write_text(
        '2026-10-11 runtime32 candidate; not a final release.\n'
        'Exit ALL Steam/Wine processes. Back up the five same-name DLLs, then copy all five together.\n'
        'Keep the existing 2-perf-gpu-auto.bat, Steam.cfg, config, userdata and games.\n'
        'steamclient.dll and steamclient64.dll must remain byte-identical to the original package.\n'
        'logs/vpsteam-runtime32.log must show install=1 in the new Steam process.\n'
        'Old vpsteamzstd64.dll may remain but is not imported by the original 64-bit client.\n'
        'For rollback, close Wine and restore all backed-up files together.\n'
        'Real full download: Purrgatory 1713610, 126782064 bytes, 56 VSZa blocks, No Error.\n'
        'This verifies that sample, not every game; long-session stability remains unverified.\n',
        encoding='utf-8')
    return manifest


# Must match the standard build.sh output, not a differently named link target.
RUNTIME_HELPER_SHA256 = RUNTIME32['vpsteamtrust32.dll']

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('original',type=Path)
    p.add_argument('helpers',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    print(json.dumps(package(args.original,args.helpers,args.output),indent=2))
