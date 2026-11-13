#!/usr/bin/env python3
"""Retire generated Linux-only build outputs from this checkout, preserving user data."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def retire(root):
    rows=[]
    raw=root/'entry/src/main/resources/rawfile'
    candidates=[]
    for name in ('linux-steam','linux-proton','linux-steam-native'):
        folder=raw/name
        if folder.is_symlink(): raise ValueError('Refusing resource directory symlink: '+str(folder))
        if folder.exists():
            for p in folder.rglob('*'):
                if p.is_symlink(): raise ValueError('Refusing resource symlink: '+str(p))
                if p.is_file(): candidates.append(p)
    for relative in ('entry/libs', 'entry/build/default/intermediates/cmake/default/obj'):
        folder=root/relative
        if folder.exists(): candidates.extend(p for p in folder.rglob('*steamohos*.so') if p.is_file())
    for p in sorted(set(candidates)):
        if p.is_symlink() or not p.resolve().is_relative_to(root.resolve()): raise ValueError('Unsafe build output '+str(p))
        rows.append(dict(path=p.relative_to(root).as_posix(),bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
        p.unlink()
    return rows

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--report',type=Path);args=parser.parse_args()
    rows=retire(ROOT)
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        previous=json.loads(args.report.read_text()) if args.report.exists() else []
        args.report.write_text(json.dumps(previous+rows,indent=2)+'\n')
    print('Retired generated Linux build outputs:',len(rows),'files,',sum(r['bytes'] for r in rows),'bytes')
if __name__=='__main__':main()
