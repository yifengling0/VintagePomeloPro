#!/usr/bin/env python3
"""Freeze actual local vendor overlays without claiming a clean CI checkout.

Every non-committed source byte is included for reconstruction. The normal CI
source gate remains strict. Revalidation rejects any effective source change.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
from verify_ci_release import git, load_app, APP_FIELDS
ROOT=Path(__file__).resolve().parents[1]

def effective(repo):
    delta=subprocess.check_output(['git','-C',str(repo),'diff','--binary','HEAD','--','.',':(exclude)**/.wraplock'])
    if len(delta)>16*1024*1024: raise ValueError('Local source delta exceeds receipt budget')
    files={}
    for raw in subprocess.check_output(['git','-C',str(repo),'ls-files','--others','--exclude-standard','-z']).split(b'\0'):
        if not raw: continue
        name=raw.decode()
        # Meson synchronization lock has no source semantics and changes on each invocation.
        if Path(name).name=='.wraplock': continue
        path=repo/name
        if path.is_symlink(): raise ValueError('Uncommitted source symlink requires a committed recipe: '+name)
        data=path.read_bytes()
        if len(data)>2*1024*1024: raise ValueError('Commit or register large local source input: '+name)
        files[name]=dict(sha256=hashlib.sha256(data).hexdigest(),data=base64.b64encode(data).decode())
    return dict(deltaSha256=hashlib.sha256(delta).hexdigest(),delta=base64.b64encode(delta).decode(),files=files)

def capture(root):
    subprocess.run(['git','-C',str(root),'diff','--quiet','HEAD','--ignore-submodules=all'],check=True)
    links={};trees={}
    def visit(repo,prefix=''):
        for record in git(repo,'ls-tree','-rz','HEAD').split('\0'):
            if not record: continue
            meta,name=record.split('\t',1);mode,_,pin=meta.split()
            if mode!='160000': continue
            child=repo/name;label=prefix+name
            if not (child/'.git').exists() or git(child,'rev-parse','HEAD')!=pin:
                raise ValueError('Local source gitlink mismatch: '+label)
            links[label]=pin;trees[label]=effective(child);visit(child,label+'/')
    visit(root)
    patches={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((root/'patches').rglob('*.patch'))}
    app=load_app(root)
    return dict(commit=git(root,'rev-parse','HEAD'),releaseTag='',app={k:app[k] for k in APP_FIELDS},
                submodules=links,patches=patches,sourceMode='local-effective-source',effectiveSources=trees)

def verify_effective(root,manifest):
    for name,expected in manifest['effectiveSources'].items():
        if git(root/name,'rev-parse','HEAD')!=manifest['submodules'][name] or effective(root/name)!=expected:
            raise ValueError('Local effective source changed during build: '+name)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);p.add_argument('--verify',type=Path);a=p.parse_args()
    if a.verify: verify_effective(ROOT,json.loads(a.verify.read_text()))
    else:
        value=capture(ROOT);a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(value,indent=2)+'\n')
    print('Local effective-source receipt PASS; clean CI acceptance is not claimed')
if __name__=='__main__':main()
