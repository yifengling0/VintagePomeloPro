# SPDX-License-Identifier: LGPL-2.1-or-later
"""Resumable, locked-content downloads for GNU installers and host preparation.

This publishes cache objects, never installed components. Callers supply trusted
HTTPS origins and a pinned byte length/SHA-256, and budget extraction space via
extra_bytes. Cancellation/network failure retains a SHA-addressed partial file.
"""
from contextlib import contextmanager
import errno
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import time
import urllib.parse
import urllib.request


class DownloadError(ValueError):
    pass


class Cancelled(DownloadError):
    pass


def checked_url(url, origins):
    try:
        parsed = urllib.parse.urlsplit(url)
        origin = parsed.scheme + '://' + parsed.netloc
        if (parsed.scheme != 'https' or not parsed.hostname or parsed.username is not None
                or parsed.password is not None or parsed.fragment or origin not in origins
                or any(ord(c) <= 32 or ord(c) >= 127 for c in url)):
            raise DownloadError('Untrusted download URL')
        _ = parsed.port
    except (TypeError, ValueError) as error:
        raise DownloadError('Untrusted download URL') from error
    return url


class OriginRedirect(urllib.request.HTTPRedirectHandler):
    def __init__(self, origins):
        self.origins = origins

    def redirect_request(self, request, response, code, message, headers, url):
        checked_url(url, self.origins)
        return super().redirect_request(request, response, code, message, headers, url)


def sha256(path, check=lambda: None):
    # open_owned rejects links, special files and foreign-owned cache objects.
    with open_owned(path, os.O_RDONLY) as stream:
        digest = hashlib.sha256()
        while block := stream.read(65536):
            check()
            digest.update(block)
        check()
        return digest.hexdigest()


@contextmanager
def open_owned(path, flags):
    path = Path(path)
    try:
        before = path.lstat()
    except FileNotFoundError:
        before = None
    if before is not None and (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1):
        raise DownloadError('Cache path is not an independent regular file')
    fd = os.open(path, flags | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
                 | getattr(os, 'O_BINARY', 0), 0o600)
    try:
        info = os.fstat(fd)
        current = path.lstat()
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                or (hasattr(os, 'geteuid') and info.st_uid != os.geteuid())
                or (info.st_dev, info.st_ino) != (current.st_dev, current.st_ino)
                or (before is not None and (before.st_dev, before.st_ino) != (info.st_dev, info.st_ino))):
            raise DownloadError('Cache file identity changed or is not owned')
        stream = os.fdopen(fd, 'rb' if flags & os.O_RDWR == 0 else 'r+b')
        fd = -1
        with stream:
            yield stream
    finally:
        if fd >= 0:
            os.close(fd)


@contextmanager
def cache_lease(path):
    # The sibling inode is stable across cache publication; never unlink it.
    with open_owned(path, os.O_RDWR | os.O_CREAT) as stream:
        if os.name == 'nt':
            import msvcrt
            if os.fstat(stream.fileno()).st_size == 0:
                stream.write(b'\0')
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)


def fetch(url, size, digest, destination, *, origins, cancelled=lambda: False,
          progress=lambda received, total: None, extra_bytes=0, reserve_bytes=64 << 20,
          timeout=30, deadline_seconds=1800, _open=None):
    """Return an existing or newly verified object, without touching live trees.

    A server ignoring Range is safely restarted from byte zero. A malformed
    partial response, encoding, size or final hash is an error, never success.
    _open is a test transport seam; production uses the redirect-checked opener.
    """
    checked_url(url, origins)
    if (type(size) is not int or not 0 < size <= 16 << 30
            or not isinstance(digest, str) or re.fullmatch('[0-9a-f]{64}', digest) is None
            or type(extra_bytes) is not int or extra_bytes < 0
            or type(reserve_bytes) is not int or reserve_bytes < 0
            or timeout <= 0 or deadline_seconds <= 0):
        raise DownloadError('Invalid locked download identity or budget')
    destination = Path(destination)
    if not destination.parent.is_dir():
        raise DownloadError('Download cache directory is missing')
    partial = destination.with_name(destination.name + '.partial.' + digest)
    end = time.monotonic() + deadline_seconds

    def check():
        if cancelled():
            raise Cancelled('Download cancelled; partial retained')
        if time.monotonic() > end:
            raise TimeoutError('Download deadline exceeded; partial retained')

    def publish():
        check()
        if sha256(partial, check) != digest:
            partial.unlink()
            raise DownloadError('Downloaded SHA-256 mismatch; partial discarded')
        check()
        # The lease excludes cooperating installers. Refuse unexpected output
        # instead of overwriting an object that appeared during the download.
        if os.path.lexists(destination):
            raise DownloadError('Cache destination appeared during download')
        os.replace(partial, destination)
        if os.name != 'nt':
            fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        progress(size, size)
        return destination

    with cache_lease(destination.with_name(destination.name + '.lock')):
        check()
        if os.path.lexists(destination):
            if destination.lstat().st_size != size or sha256(destination, check) != digest:
                raise DownloadError('Existing cache identity mismatch')
            progress(size, size)
            return destination
        with open_owned(partial, os.O_RDWR | os.O_CREAT) as output:
            offset = os.fstat(output.fileno()).st_size
            if offset > size:
                raise DownloadError('Partial cache object exceeds locked size')
            required = size - offset + extra_bytes + reserve_bytes
            if shutil.disk_usage(destination.parent).free < required:
                raise OSError(errno.ENOSPC, 'Insufficient download/extraction space', str(destination.parent))
            if offset < size:
                opener = _open or urllib.request.build_opener(OriginRedirect(origins)).open
                headers = {'Accept-Encoding': 'identity'}
                if offset:
                    headers['Range'] = 'bytes=' + str(offset) + '-'
                request = urllib.request.Request(url, headers=headers)
                check()
                with opener(request, timeout=min(timeout, max(.001, end - time.monotonic()))) as response:
                    checked_url(response.geturl(), origins)
                    status = response.status
                    if status == 206:
                        if not offset:
                            raise DownloadError('Unexpected partial response without a Range request')
                        expected = 'bytes ' + str(offset) + '-' + str(size - 1) + '/' + str(size)
                        if response.headers.get('Content-Range') != expected:
                            raise DownloadError('Invalid resumed Content-Range')
                    elif status == 200:
                        if response.headers.get('Content-Range') is not None:
                            raise DownloadError('Unexpected Content-Range')
                        offset = 0
                        # Restarting consumes the bytes formerly in the partial.
                        if shutil.disk_usage(destination.parent).free + os.fstat(output.fileno()).st_size < size + extra_bytes + reserve_bytes:
                            raise OSError(errno.ENOSPC, 'Insufficient space to restart download')
                    else:
                        raise DownloadError('Unexpected download status: ' + str(status))
                    if response.headers.get('Content-Encoding', 'identity').lower() != 'identity':
                        raise DownloadError('Encoded response cannot satisfy byte ranges')
                    length = response.headers.get('Content-Length')
                    if length is not None and (not length.isdecimal() or int(length) != size - offset):
                        raise DownloadError('Download Content-Length mismatch')
                    if status == 200:
                        output.seek(0)
                        output.truncate()
                    else:
                        output.seek(offset)
                    received = offset
                    try:
                        progress(received, size)
                        while True:
                            check()
                            block = response.read(min(65536, size - received + 1))
                            if not block:
                                break
                            if received + len(block) > size:
                                raise DownloadError('Downloaded bytes exceed locked size')
                            output.write(block)
                            received += len(block)
                            progress(received, size)
                        if received != size:
                            raise DownloadError('Download interrupted; partial retained')
                    finally:
                        output.flush()
                        os.fsync(output.fileno())
            else:
                # A process could have died after writing its last byte but
                # before syncing or publishing the complete partial object.
                os.fsync(output.fileno())
        return publish()
