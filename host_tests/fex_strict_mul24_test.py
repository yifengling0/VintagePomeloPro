#!/usr/bin/env python3
"""Compile the actual guarded integer kernel and differential against SoftFloat."""
from pathlib import Path
import argparse,shutil,subprocess,tempfile

def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser()
    p.add_argument('--fex-src',type=Path,default=root/'thirdparty/fex')
    p.add_argument('--cc',default='cc')
    args=p.parse_args()
    src=args.fex_src.resolve()
    sf=src/'External/SoftFloat-3e'
    if not sf.is_dir():p.error('A materialized pinned FEX source is required')
    with tempfile.TemporaryDirectory(prefix='strict-mul24-') as tmp:
        work=Path(tmp)
        common=work/'FEXCore/Source/Common'
        common.mkdir(parents=True)
        shutil.copyfile(src/'FEXCore/Source/Common/SoftFloat.h',common/'SoftFloat.h')
        if (src/'FEXCore/Source/Common/StrictMul24.h').is_file():
            shutil.copyfile(src/'FEXCore/Source/Common/StrictMul24.h',common/'StrictMul24.h')
        patch=root/'scripts/patches/fex-wow64-strict-mul24.patch'
        reverse=subprocess.run(['patch','-d',str(work),'-p1','-R','--dry-run','-s'],
            stdin=patch.open('rb'),capture_output=True)
        if reverse.returncode:
            subprocess.run(['patch','-d',str(work),'-p1','-s'],stdin=patch.open('rb'),check=True)
        subprocess.run(['patch','-d',str(work),'-p1','-R','--dry-run','-s'],stdin=patch.open('rb'),check=True)
        exe=work/'test'
        files=[sf/'src'/f'{name}.c' for name in ('extF80_mul','s_roundPackToExtF80',
            's_normSubnormalExtF80Sig','s_propagateNaNExtF80UI','softfloat_raiseFlags')]
        subprocess.run([args.cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-DFEXCORE_PRESERVE_ALL_ATTR=','-DSOFTFLOAT_FAST_INT64=1','-DSOFTFLOAT_BUILTIN_CLZ=1',
            '-DINLINE=static inline','-DINLINE_LEVEL=4','-I'+str(common),
            '-I'+str(sf/'include'),'-I'+str(sf/'include/SoftFloat-3e'),
            str(root/'smoke/fex_strict_mul24_test.c'),*[str(f) for f in files],'-o',str(exe)],check=True,timeout=60)
        out=subprocess.run([str(exe)],capture_output=True,text=True,check=True,timeout=60)
        assert 'checked=2400000' in out.stdout and 'failures=0 complete=1' in out.stdout,out.stdout
        print(out.stdout.strip())

if __name__=='__main__':main()
