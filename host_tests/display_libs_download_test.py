"""Exercise display-library retries, verified mirrors and interrupted caches."""
import hashlib
import io
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = (ROOT / 'scripts/build_display_libs.sh').read_text()
FUNCTION = SCRIPT[SCRIPT.index('fetch_and_unpack() {'):SCRIPT.index('\nPIXMAN_VER=')]


class DisplayDownloadTest(unittest.TestCase):
    def test_failed_primary_and_bad_cached_archive_use_verified_mirror(self):
        with tempfile.TemporaryDirectory(prefix='display-download-') as directory:
            root = Path(directory)
            build = root / 'build'
            downloads = build / 'downloads'
            downloads.mkdir(parents=True)
            archive = root / 'valid.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                data = b'verified pixman source'
                member = tarfile.TarInfo('pixman/file.txt')
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
            checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
            (downloads / 'pixman.tar.gz').write_bytes(b'interrupted download')
            tools = root / 'bin'
            tools.mkdir()
            curl = tools / 'curl'
            curl.write_text('''#!/bin/bash
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then target="$2"; shift 2; else url="$1"; shift; fi
done
if [ "$url" = primary/pixman.tar.gz ]; then printf partial > "$target"; exit 35; fi
cp "$FIXTURE_ARCHIVE" "$target"
''')
            curl.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'],
                       FIXTURE_ARCHIVE=str(archive))
            command = (f'BUILD_DIR={shlex.quote(str(build))}\nDL="$BUILD_DIR/downloads"\n'
                       'log() { :; }\nerr() { echo "$*" >&2; exit 1; }\n' + FUNCTION +
                       f'\nfetch_and_unpack primary/pixman.tar.gz {checksum} pixman mirror/pixman.tar.gz\n')
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', command], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((downloads / 'pixman.tar.gz').read_bytes(), archive.read_bytes())
            self.assertEqual((build / 'pixman/file.txt').read_bytes(), data)
            self.assertEqual(list(downloads.glob('*.part.*')), [])


if __name__ == '__main__':
    unittest.main()
