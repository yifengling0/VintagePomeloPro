#!/usr/bin/env python3
"""Content identity for compiler/sysroot closures used by GNU component caches."""
import hashlib
from pathlib import Path
def tree_identity(paths):
 h=hashlib.sha256()
 for label,root in paths:
  root=Path(root).resolve(strict=True)
  for p in sorted(root.rglob('*')) if root.is_dir() else [root]:
   if not p.is_file():continue
   h.update((label+':'+(p.relative_to(root).as_posix() if root.is_dir() else p.name)+'\0').encode())
   with p.open('rb') as f:
    for part in iter(lambda:f.read(1048576),b''):h.update(part)
 return h.hexdigest()


def compiler_identity(compiler):
 import shutil,subprocess
 cc=Path(shutil.which(str(compiler)) or compiler).resolve(strict=True)
 machine=subprocess.check_output([cc,'-dumpmachine'],text=True).strip()
 def locate(flag):
  value=subprocess.check_output([cc,flag],text=True).strip()
  return Path(shutil.which(value) or value).resolve(strict=True)
 cxx=cc.with_name(cc.name.replace('gcc','g++'))
 include=locate('-print-file-name=include');libc=locate('-print-file-name=libc.so')
 roots=[('cc',cc),('cxx',cxx),('ld',locate('-print-prog-name=ld')),('as',locate('-print-prog-name=as')),
        ('compiler-backends',locate('-print-prog-name=cc1').parent),
        ('libc',libc.parent),('internal-headers',include)]
 target=Path('/usr')/machine/'include'
 if target.exists():roots.append(('target-headers',target))
 for candidate in (Path('/usr/include/c++'),Path('/usr/include')/machine/'c++'):
  if candidate.exists():roots.append(('cxx-headers:'+candidate.name,candidate))
 return dict(machine=machine,closure=tree_identity(roots))
