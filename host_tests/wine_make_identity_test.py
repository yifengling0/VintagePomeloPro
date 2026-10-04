"""The actual top-level Wine stamp recipe must check identity even with newer stamps."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='wine-make-identity-') as directory:
    temp=Path(directory);build=temp/'build';stamps=build/'.stamps';stamps.mkdir(parents=True)
    source=temp/'source';source.mkdir();(source/'configure.ac').write_text('source\n')
    native=build/'wine-native';native.mkdir();makefile=native/'Makefile'
    makefile.write_text(f'srcdir = {temp / "wrong-source"}\n')
    sentinel=build/'wine.so';sentinel.write_text('old compiled output\n')
    stamp=stamps/'wine-x86_64-x86_64';stamp.write_text('old stamp\n')
    deps=stamps/'deps';deps.touch()
    # This exact state previously took the stamp fast-path and never read srcdir.
    for path in (stamp,sentinel,deps):os.utime(path,(time.time()+86400,time.time()+86400))
    sdk=temp/'sdk';(sdk/'native/llvm/bin').mkdir(parents=True)
    clang=sdk/'native/llvm/bin/clang';clang.write_text('#!/bin/sh\necho fake-test-clang-version\n');clang.chmod(0o755)
    mingw=temp/'mingw';(mingw/'bin').mkdir(parents=True)
    for name,text in [('arm64ec-w64-mingw32-clang','exit 0'),('llvm-ar','echo obj.arm64ec/member')]:
        path=mingw/'bin'/name;path.write_text('#!/bin/sh\n'+text+'\n');path.chmod(0o755)
    def snapshot():return {str(p.relative_to(temp)):p.read_bytes() for p in temp.rglob('*') if p.is_file()}
    command=['make','-f',str(ROOT/'Makefile'),'-o',str(deps),'wine',f'BUILD_DIR={build}',
             f'STAMPS={stamps}',f'WINE_SRC={source}',f'WINE_SENTINEL={sentinel}',
             'NATIVE_ARCH=x86_64','WINE_ARCH=x86_64','GUEST_ARCH=x86_64']
    environment={**os.environ,'OHOS_SDK':str(sdk),'LLVM_MINGW':str(mingw),'HOST_OS':'Linux'}
    for expected in ('cached srcdir mismatch','no provenance manifest'):
        before=snapshot()
        result=subprocess.run(command,cwd=ROOT,env=environment,capture_output=True,text=True,timeout=30)
        assert result.returncode!=0 and expected in result.stderr,result.stdout+result.stderr
        assert '[wine] up to date' not in result.stdout and '=== wine (' not in result.stdout
        assert snapshot()==before,'top-level identity check changed source, stamps, cache or tool fixtures'
        makefile.unlink(missing_ok=True)
print('Top-level Wine stamp: foreign srcdir and missing provenance reject despite newer stamp/sentinel; all files preserved')
