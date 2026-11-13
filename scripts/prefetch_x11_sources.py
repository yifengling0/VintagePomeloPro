#!/usr/bin/env python3
"""Prefetch and validate the X11 source archives used by the build scripts.

The build scripts remain the authority for URLs and checksums. Refuse unknown
call syntax so a newly added download cannot silently escape the CI preflight.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ('build_xcb_stack.sh', 'build_x11_client.sh', 'build_xwayland.sh')
CALL = re.compile(r'^\s*fetch_and_unpack\s+"([^"\n]+)"\s*\\\s*\n\s*"([0-9a-f]{64})"', re.M)
SHA1_URL = 'https://github.com/dottedmag/libsha1/archive/$LIBSHA1_SHA.tar.gz'
EXT_URL = 'https://xorg.freedesktop.org/archive/individual/lib/$package.tar.xz'


def sources(root=ROOT):
    result = {}
    for name in SCRIPTS:
        content = (root / 'scripts' / name).read_text()
        calls = re.findall(r'^\s*fetch_and_unpack\s+', content, re.M)
        direct = CALL.findall(content)
        dynamic_count = 1 if name == 'build_x11_client.sh' else 0
        if len(calls) != len(direct) + dynamic_count:
            raise ValueError('Unrecognized source download declaration: ' + name)
        for url, digest in direct:
            if url == SHA1_URL:
                match = re.search(r'^LIBSHA1_SHA="([0-9a-f]{40})"$', content, re.M)
                if not match: raise ValueError('Missing pinned libsha1 revision')
                url = url.replace('$LIBSHA1_SHA', match.group(1))
            if '$' in url: raise ValueError('Unresolved source URL: ' + url)
            if url in result and result[url] != digest: raise ValueError('Conflicting source digest: ' + url)
            result[url] = digest
        if dynamic_count:
            if content.count('fetch_and_unpack "' + EXT_URL + '" "$checksum" "$package"') != 1:
                raise ValueError('Unknown X11 extension download declaration')
            match = re.search(r"done <<'X11_EXTENSIONS'\n(.*?)\nX11_EXTENSIONS", content, re.S)
            if not match: raise ValueError('Missing X11 extension inventory')
            for row in match.group(1).splitlines():
                parts = row.split()
                if len(parts) != 4 or not re.fullmatch(r'[A-Za-z0-9._+-]+', parts[0]) or not re.fullmatch(r'[0-9a-f]{64}', parts[3]):
                    raise ValueError('Invalid X11 extension inventory entry')
                result[EXT_URL.replace('$package', parts[0])] = parts[3]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--list', action='store_true', help='Print pinned sources without network access')
    args = parser.parse_args()
    entries = sources()
    if args.list:
        for url, digest in sorted(entries.items()): print(digest, url)
        return
    args.downloads.mkdir(parents=True, exist_ok=True)
    helper = ROOT / 'scripts/download_verified_source.sh'
    command = 'set -euo pipefail; source "$1"; log() { echo "$*"; }; err() { echo "$*" >&2; return 1; }; download_verified_source "$2" "$3" "$4"'
    for url, digest in sorted(entries.items()):
        name = url.rsplit('/', 1)[-1]
        print('Verifying X11 source:', name, flush=True)
        subprocess.run(['bash', '-c', command, 'prefetch', str(helper), url, digest, str(args.downloads / name)], check=True)

if __name__ == '__main__':
    main()
