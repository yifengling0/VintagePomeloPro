#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pinned build-only LLVM/multilib headers; adapted from HSL b001b25.
No system package installation. Existing verified archives and files are reused.
"""
import hashlib,json,subprocess,tarfile,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
 return h.hexdigest()
def fetch(entry,cache):
 cache.mkdir(parents=True,exist_ok=True);target=cache/entry['file']
 if target.exists():
  if target.stat().st_size==entry['bytes'] and sha(target)==entry['sha256']:return target
  raise ValueError('Existing tooling archive identity mismatch: '+entry['file'])
 pending=target.with_suffix('.pending')
 try:
  with urllib.request.urlopen(entry['url'],timeout=45) as response,pending.open('wb') as out:
   total=0
   while True:
    chunk=response.read(1048576)
    if not chunk:break
    total+=len(chunk)
    if total>entry['bytes']:raise ValueError('Tooling download budget exceeded')
    out.write(chunk)
  if total!=entry['bytes'] or sha(pending)!=entry['sha256']:raise ValueError('Tooling archive checksum mismatch')
  pending.replace(target)
 finally:pending.unlink(missing_ok=True)
 return target
def extract(entry,work,cache):
    """Keep headers and shared link inputs, never LLVM's large static archives."""
    archive = fetch(entry, cache)
    stamp = work/(entry['file']+'.json')
    deps = work/'deps'
    if stamp.is_file() and json.loads(stamp.read_text()).get('package') == entry:
        old = json.loads(stamp.read_text())
        if all((deps/name).is_file() and sha(deps/name) == digest
               for name, digest in old['files'].items()):
            return
    files = {}
    links = []
    process = subprocess.Popen(['dpkg-deb', '--fsys-tarfile', archive], stdout=subprocess.PIPE)
    try:
        total = 0
        with tarfile.open(fileobj=process.stdout, mode='r|') as source:
            for member in source:
                part = Path(member.name.removeprefix('./'))
                if part.is_absolute() or '..' in part.parts:
                    raise ValueError('Unsafe build dependency path')
                name = part.as_posix()
                if not ('/include/' in name or name.endswith('/libclang-cpp.so.15')
                        or '/libX11.so.' in name):
                    continue
                target = deps/part
                if member.isfile():
                    total += member.size
                    if total > 400 << 20:
                        raise ValueError('Build dependency extraction budget')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if target.is_symlink():
                        target.unlink()
                    with source.extractfile(member) as stream:
                        data = stream.read()
                    target.write_bytes(data)
                    files[name] = hashlib.sha256(data).hexdigest()
                elif member.issym():
                    link = Path(member.linkname)
                    if link.is_absolute() or not (target.parent/link).resolve().is_relative_to(deps.resolve()):
                        raise ValueError('Unsafe build dependency link')
                    links.append((target, link))
        while process.stdout.read(65536):
            pass
        if process.wait(timeout=30):
            raise ValueError('Build dependency extraction failed')
        for target, link in links:
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.is_symlink():
                target.unlink()
            elif target.exists():
                raise ValueError('Preserve unexpected build dependency file')
            target.symlink_to(link)
        stamp.write_text(json.dumps(dict(package=entry, files=files), indent=2)+'\n')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)


def prepare(work,cache):
 lock=json.loads((ROOT/'docs/linux-steam/fex-gl-tooling.lock.json').read_text())
 work.mkdir(parents=True,exist_ok=True)
 for entry in lock['packages']:extract(entry,work,cache)
 return work/'deps'
