#!/usr/bin/env python3
"""Build locked upgraded Wine in isolation with the existing OHOS build functions.

Reuses dependency installs, never copies Wine objects or changes production pins.
The output remains a component-test candidate until package/device acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time
from prepare_fex_upstream import ROOT


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else streamed(stream)


def streamed(stream):
    h=hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024*1024), b''): h.update(chunk)
    return h.hexdigest()


def git(path, *args):
    return subprocess.check_output(['git','-c','gc.auto=0','-C',str(path),*args]).decode().strip()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--dependencies',type=Path,required=True)
    p.add_argument('--llvm-mingw',type=Path,required=True)
    p.add_argument('--container',required=True)
    p.add_argument('--container-root',default='/data/src/winehua')
    p.add_argument('--stage',choices=('core','full'),default='core')
    p.add_argument('--jobs',type=int,default=8)
    args=p.parse_args()
    source,build,deps,mingw=(x.resolve() for x in (args.source,args.build,args.dependencies,args.llvm_mingw))
    if not all(x.is_relative_to(ROOT/'workspace_temp') for x in (source,build)):
        raise ValueError('Candidate source/build must remain in isolated workspace_temp')
    lockpath=ROOT/'docs/upstream-sync/wine-candidate.lock.json'
    lock=json.loads(lockpath.read_text())
    if git(source,'rev-parse','HEAD')!=lock['adaptedCommit'] or git(source,'write-tree')!=lock['adaptedTree']:
        raise ValueError('Wine candidate differs from the locked commit/tree')
    if git(source,'status','--porcelain','--untracked-files=all'):
        raise ValueError('Unregistered Wine candidate source changes')
    def mapped(path):
        return Path(args.container_root)/path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
    q=lambda x:shlex.quote(str(x))
    # Record exact shared headers/libraries without copying the dependency tree.
    inputs={}
    dep_root=deps/'sysroot-ext/usr'
    for path in sorted(dep_root.rglob('*')):
        if path.is_file():inputs[str(path.relative_to(deps))]=digest(path)
    if not inputs:raise ValueError('Existing dependency install is required')
    driver=ROOT/'scripts/build_wine.sh'
    flags=driver.read_text()
    start=flags.index('WINE_CFLAGS=')
    end=flags.index('# ---- main ----',start)
    functions=flags[start:end]
    needle='"$srv_cflags" "$pie_mode";'
    if functions.count(needle)!=1:raise ValueError('Unknown standalone Wine server build contract')
    functions=functions.replace(needle,'"$srv_cflags" "$pie_mode" "$VPP_CANDIDATE_INPUT_SHA";')
    toolmeta=subprocess.check_output(['docker','exec',args.container,'bash','-c',
        shlex.join([str(mapped(mingw)/'bin/clang'),'--version'])+'\n'+
        '/apps/harmony/sdk/default/openharmony/native/llvm/bin/clang --version']).decode()
    toolhashes=subprocess.check_output(['docker','exec',args.container,'bash','-c',
        'sha256sum /apps/harmony/sdk/default/openharmony/native/llvm/bin/{clang,ld.lld,llvm-ar,llvm-strip} '+
        q(mapped(mingw)/'bin/clang')+' '+q(mapped(mingw)/'bin/ld.lld')]).decode()
    identity=dict(sourceTree=lock['adaptedTree'],buildScript=digest(driver),envScript=digest(ROOT/'scripts/env.sh'),
        entry=digest(Path(__file__)),mingwClang=digest(mingw/'bin/clang'),toolVersions=toolmeta,
        toolExecutableHashes=toolhashes,
        dependencies=inputs,architecture='OHOS AArch64 + ARM64EC/AArch64/i386 PE',configuration='production -g -O2; no relaxed sync/precision')
    fingerprint=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
    build.mkdir(parents=True,exist_ok=True)
    previous=build/'inputs.json'
    pending=build/'inputs-pending.json'
    changed=pending.exists() or (previous.exists() and json.loads(previous.read_text())['sha256']!=fingerprint)
    pending.write_text(json.dumps(dict(sha256=fingerprint,identity=identity),indent=2)+'\n')
    src,out,dependency_root,toolchain=map(mapped,(source,build,deps,mingw))
    modules='ntdll win32u winevulkan opengl32 winewayland.drv winex11.drv wineohos.drv winegstreamer'
    lines=['set -euo pipefail','SCRIPT_DIR='+q(Path(args.container_root)/'scripts'),
        'export WINE_SRC='+q(src),'export BUILD_DIR='+q(dependency_root),
        'export WINE_BUILD_ROOT='+q(out),'export LLVM_MINGW='+q(toolchain),
        'export WINE_ARCH=aarch64 NATIVE_ARCH=arm64-v8a JOBS='+str(args.jobs),
        'source "$SCRIPT_DIR/env.sh"','NATIVE_LIBS='+q(out/'staged'),
        'WINE_UNIX_MODULES='+q(modules if args.stage=='core' else ''),'WINE_PE_MODULES=""',
        'VPP_CANDIDATE_INPUT_SHA='+q(fingerprint),'CONFIGURE_BIN='+q(src/'configure'),functions]
    if changed:
        lines+=['for directory in "$WINE_BUILD_ROOT/wine-ohos-aarch64" "$WINE_BUILD_ROOT/wine-native"; do',
          'if [ -f "$directory/Makefile" ]; then make -C "$directory" -o Makefile clean; rm -f "$directory/Makefile"; fi','done']
    lines+=['build_native_tools','build_ohos_unix','build_wineserver']
    script='\n'.join(lines)+'\n'
    logpath=build/'compile.log';started=time.monotonic()
    with logpath.open('wb') as log:
        proc=subprocess.Popen(['docker','exec','-i',args.container,'bash','-s'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        proc.stdin.write(script.encode());proc.stdin.close();written=0;tail=b''
        for chunk in iter(lambda:proc.stdout.read(65536),b''):
            if written+len(chunk)>32*1024*1024:
                log.seek(0);log.truncate();written=0
            log.write(chunk);log.flush();written+=len(chunk);tail=(tail+chunk)[-65536:]
        code=proc.wait()
    if code:
        raise RuntimeError('Wine candidate build failed: '+tail.decode(errors='replace')[-5000:])
    outputs={}
    for module in modules.split():
        path=build/'wine-ohos-aarch64/dlls'/module/(module.removesuffix('.drv')+'.so')
        data=path.read_bytes()[:20]
        if data[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little')!=183:
            raise ValueError('Wrong Wine Unix architecture: '+module)
        outputs[str(path.relative_to(build))]=dict(bytes=path.stat().st_size,sha256=digest(path))
    server=build/'wine_server-aarch64/libwineserver.so'
    outputs[str(server.relative_to(build))]=dict(bytes=server.stat().st_size,sha256=digest(server))
    if args.stage=='full':
        wine=build/'wine-ohos-aarch64'
        for path in sorted(wine.rglob('*')):
            if path.is_file() and (path.suffix in ('.dll','.exe','.drv','.sys','.acm','.ax','.ocx','.tlb','.msstyles','.cpl','.so','.nls','.winmd')
                                   or path==wine/'loader/wine.inf'):
                outputs[str(path.relative_to(build))]=dict(bytes=path.stat().st_size,sha256=digest(path))
    result=dict(schemaVersion=1,policy='component-test-candidate',stage=args.stage,upstreamCommit=lock['upstream']['commit'],
        adaptedCommit=lock['adaptedCommit'],adaptedTree=lock['adaptedTree'],inputSha256=fingerprint,outputs=outputs,
        seconds=round(time.monotonic()-started,2),productionPinsChanged=False,deviceValidated=False)
    (build/('wine-'+args.stage+'-build.json')).write_text(json.dumps(result,indent=2)+'\n')
    pending.replace(previous)
    print(json.dumps({k:v for k,v in result.items() if k!='outputs'}),flush=True)


if __name__=='__main__':main()
