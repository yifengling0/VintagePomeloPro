"""Read-only identity check for the exact frozen Steam client and Zstd overlay.

Run on Windows: python audit_install.py C:/path/to/Steam
This does not prove which DLL a running process has loaded. Cold-restart Wine
before testing the directory. No account/config/CDN data is read.
"""
import argparse
import hashlib
import json
from pathlib import Path

ORIGINAL = {
    'steamclient.dll': '95a7e01a64e20f0726b63cfb63b5240e51c7b15940b52d03e8da4c412ca54669',
    'steamclient64.dll': 'a2ad498763c4822fd4555c2cba4d4475a2ba450e403a774c1366826ad9451dd1',
    'SteamUI.dll': 'f08296fb1345e489f97f7d36a2740d899e8a3a3aca0537e300bc9fda0e5e45f1',
}
V3 = {
    'steamclient.dll': '9c45de0b9a5fcc67e6bbfb28f54288da0b19f60332a750944dfc58e6aebddecd',
    'steamclient64.dll': 'bb7878cf75b3878d3c365572e0985b69d5850b39dd00e2d3e27e0157246324f9',
    'SteamUI.dll': '88004c47d74bf6cd25c2abd191db0eff07d7f5a29cab49b937d6581b73a07600',
    'vpsteamtrust32.dll': '6551662acfa1cebab23f82909b0afa6d15289d7d1550f36d0571cb9ff01920b1',
    'vpsteamzstd32.dll': '57c1b0503d131853aa899a896d3a0077b9f62a44ec5545dd6ce58e0b798e0faf',
    'vpsteamzstd64.dll': 'e339ce8195a6fdffdd627a1c528fbacc0058fb6d4db7f35e2a2215fecf5c135b',
}
V2 = dict(V3, **{'vpsteamzstd64.dll': '9d561b3033904e240c89e7eab1d2a9585fa6b4ee3cdaf649bd387d01319b8c84'})


def classify(hashes):
    if all(hashes.get(n) == h for n, h in V3.items()):
        return 'v3-candidate-complete'
    if all(hashes.get(n) == h for n, h in V2.items()):
        return 'v2-complete-not-a-stable-release'
    if all(hashes.get(n) == h for n, h in ORIGINAL.items()):
        return 'original-no-zstd-support'
    return 'mixed-missing-or-unknown-files'


def audit(directory):
    hashes = {}
    for name in V3:
        path = directory/name
        try:
            with path.open('rb') as f:
                h = hashlib.sha256()
                for block in iter(lambda: f.read(1 << 20), b''):
                    h.update(block)
            hashes[name] = h.hexdigest()
        except FileNotFoundError:
            hashes[name] = None
    return {'state': classify(hashes), 'sha256': hashes,
            'runtimeLoadedIdentityVerified': False,
            'downloadCompletionVerified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error('Steam directory does not exist')
    print(json.dumps(audit(args.directory), indent=2))
