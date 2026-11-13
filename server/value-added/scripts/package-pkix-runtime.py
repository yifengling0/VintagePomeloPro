#!/usr/bin/env python3
"""Stage pinned Alpine OpenSSL files from apk for a cloud-function ZIP; no secrets."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
args = parser.parse_args()
expected = {
    'ld-musl-x86_64.so.1': '38d022ce7425ff105ccfb53598f606e6e5f5f0a34bfbc793d65e6f34c9d72806',
    'libcrypto.so.3': 'bb2d86be6845456696f90f0948b6de6369f66612b7cf3ffa755e664436fdbc88',
    'libssl.so.3': 'ed8d30405217b3904a8df9ac311002d0ae43df516e60c5b3a8a998eda68ee340',
    'openssl': '28def3c5ebd2a6e3a6e5e7d11ec629ae92769b518feb18193b0745620bbda7ab',
}
for name, digest in expected.items():
    if hashlib.sha256((args.source / name).read_bytes()).hexdigest() != digest:
        raise SystemExit('Pinned PKIX runtime hash mismatch: ' + name)
args.destination.mkdir(parents=True, exist_ok=True)
for name in expected:
    shutil.copyfile(args.source / name, args.destination / name)
    (args.destination / name).chmod(0o755)
wrapper = '''#!/bin/sh
set -eu
pkix_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export OPENSSL_CONF=/dev/null
exec "$pkix_dir/ld-musl-x86_64.so.1" --library-path "$pkix_dir" "$pkix_dir/openssl" "$@"
'''
(args.destination / 'openssl-verify').write_text(wrapper, encoding='utf-8')
(args.destination / 'openssl-verify').chmod(0o755)
expected['openssl-verify'] = hashlib.sha256(wrapper.encode()).hexdigest()
(args.destination / 'manifest.json').write_text(json.dumps({
    'protocol': 1, 'opensslVersion': '3.5.9', 'source': 'Alpine signed apk openssl/libssl3/libcrypto3 3.5.9-r0',
    'files': expected}, sort_keys=True, indent=2) + '\n', encoding='utf-8')
print('Pinned PKIX runtime staged:', args.destination)
