#!/usr/bin/env python3
"""Build the Huawei single-callback function ZIP without account or payment credentials."""
import argparse
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import hashlib
import json

parser = argparse.ArgumentParser()
parser.add_argument('--clouddb-evidence', type=Path, required=True)
parser.add_argument('--pkix-runtime', type=Path)
parser.add_argument('--iap-evidence', type=Path)
parser.add_argument('--membership-release-evidence', type=Path)
parser.add_argument('--renewal-evidence', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1]
evidence = json.loads(args.clouddb_evidence.read_text())
for field in ['sameBaseConflict', 'duplicateOrder', 'callbackAbort', 'serverRollback', 'ownerImmutable', 'disconnectedFailsClosed']:
    if evidence.get(field) is not True:
        raise SystemExit('CloudDB evidence incomplete: ' + field)
if not (source / 'node_modules/@agconnect/database-server').is_dir():
    raise SystemExit('Install pinned dependencies with npm ci first')
package = json.loads((source / 'package.json').read_text())
package.update({'type': 'commonjs', 'main': 'handler.js'})
args.output.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(args.output, 'w', ZIP_DEFLATED) as z:
    z.writestr('package.json', json.dumps(package))
    z.write(source / 'handler.cjs', 'handler.js')
    z.write(source / 'package-lock.json', 'package-lock.json')
    z.write(args.clouddb_evidence, 'config/clouddb-verification.json')
    z.write(source / 'config/product-catalog.json', 'config/product-catalog.json')
    if args.iap_evidence:
        report = json.loads(args.iap_evidence.read_text())
        if report.get('queryAndSignatureVerified') is not True:
            raise SystemExit('IAP evidence incomplete')
        z.write(args.iap_evidence, 'config/iap-verification.json')
    for path, name, fields in [
        (args.membership_release_evidence, 'membership-release.json', ['normalPaymentVerified', 'backupAccepted',
            'supportVerified', 'cancelInAppVerified', 'agreementsReadVerified']),
        (args.renewal_evidence, 'renewal-verification.json', ['timerActive', 'deviceDisplayed', 'deepLinkOpened',
            'subscriptionCategoryEligible', 'receiptVerified', 'paginationVerified', 'advanceNoticeVerified',
            'cancelStopsVerified', 'permissionClosedVerified', 'dedupVerified', 'retryAlarmVerified'])]:
        if path:
            report = json.loads(path.read_text())
            allowed = set(fields) | {'operatorName', 'appId', 'projectId', 'environment', 'oneTimeProductId', 'testedAt'}
            if set(report) - allowed or any(report.get(field) is not True for field in fields):
                raise SystemExit('Incomplete or unexpected release evidence: ' + name)
            z.write(path, 'config/' + name)
    for folder in ['src', 'node_modules']:
        for path in sorted((source / folder).rglob('*')):
            if path.is_file():
                if any(part in ('.git', 'workspace_temp') for part in path.relative_to(source).parts):
                    raise SystemExit('Unexpected private/development path')
                z.write(path, path.relative_to(source).as_posix())
    if args.pkix_runtime:
        manifest = json.loads((args.pkix_runtime / 'manifest.json').read_text())
        expected = {'openssl-verify', 'openssl', 'ld-musl-x86_64.so.1', 'libssl.so.3', 'libcrypto.so.3'}
        if set(manifest.get('files', {})) != expected:
            raise SystemExit('PKIX runtime incomplete')
        for name in sorted(expected):
            path = args.pkix_runtime / name
            if hashlib.sha256(path.read_bytes()).hexdigest() != manifest['files'][name]:
                raise SystemExit('PKIX runtime integrity failure')
            z.write(path, 'runtime/' + name)
        z.write(args.pkix_runtime / 'manifest.json', 'runtime/manifest.json')
print(json.dumps({'file': str(args.output), 'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
    'bytes': args.output.stat().st_size, 'containsCredentials': False, 'iapProbeIncluded': bool(args.iap_evidence)}))
