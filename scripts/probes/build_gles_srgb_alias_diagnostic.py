#!/usr/bin/env python3
"""Build an opt-in native alias diagnostic without editing production objects.

Run inside the existing OHOS build container after building VirGL normally.
The output library must only be used in an experiment package. No caps change.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo',type=Path,default=Path('/data/src/winehua'))
    parser.add_argument('--build',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    repo=args.repo.resolve()
    build=(args.build or repo/'build/native_arm64-v8a/virglrenderer').resolve()
    output=(args.output or repo/'workspace_temp/srgb-alias-diagnostic-20261009').resolve()
    output.mkdir(parents=True,exist_ok=True)
    original=repo/'thirdparty/virglrenderer/src/vrend/vrend_renderer.c'
    before=original.read_bytes()
    text=before.decode()
    marker='int vrend_renderer_init(const struct vrend_if_cbs *cbs, uint32_t flags)'
    call='   init_features(gles ? 0 : gl_ver,\n                 gles ? gl_ver : 0);'
    assert text.count(marker)==1 and text.count(call)==1,'renderer layout changed'
    source=output/'vrend_renderer.c'
    text=text.replace(marker,'#include "'+str(repo/'scripts/probes/gles_srgb_alias_probe.c')+'"\n\n'+marker)
    text=text.replace(call,call+'\n\n   if (gles) winehua_srgb_alias_probe();')
    source.write_text(text)
    commands=json.loads((build/'compile_commands.json').read_text())
    command=next(c for c in commands if c['file'].endswith('/vrend_renderer.c'))
    compile_args=shlex.split(command['command'])
    obj=output/Path(command['output']).name
    compile_args[compile_args.index('-o')+1]=str(obj)
    compile_args[compile_args.index('-c')+1]=str(source)
    # Headers beside the original renderer, including generated include paths.
    compile_args+=['-I'+str(original.parent)]
    for flag in ('-MQ','-MF'):
        if flag in compile_args:
            compile_args[compile_args.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
    subprocess.run(compile_args,cwd=build,check=True)
    archive=output/'libvirgl-alias-diagnostic.a'
    ar=Path(compile_args[0]).with_name('llvm-ar')
    if archive.exists(): archive.unlink()
    mri=f'CREATE {archive}\nADDLIB {build / "src/libvirgl.a"}\nSAVE\nEND\n'
    subprocess.run([str(ar),'-M'],input=mri,text=True,check=True)
    subprocess.run([str(ar),'r',str(archive),str(obj)],check=True)
    link=subprocess.check_output(['ninja','-t','commands','src/libvirglrenderer.so.1.11.0'],cwd=build,text=True).splitlines()[-1]
    link_args=shlex.split(link)
    lib=output/'libvirglrenderer.so.1.11.0'
    link_args[link_args.index('-o')+1]=str(lib)
    assert 'src/libvirgl.a' in link_args
    link_args[link_args.index('src/libvirgl.a')]=str(archive)
    subprocess.run(link_args,cwd=build,check=True)
    assert original.read_bytes()==before,'production source was changed'
    proof=dict(diagnosticOnly=True,productionCapsUnchanged=True,
        productionSourceUnchanged=True,
        productionRendererSha256=hashlib.sha256((build/'src/libvirglrenderer.so.1.11.0').read_bytes()).hexdigest(),
        diagnosticRendererSha256=hashlib.sha256(lib.read_bytes()).hexdigest(),
        probeSourceSha256=hashlib.sha256((repo/'scripts/probes/gles_srgb_alias_probe.c').read_bytes()).hexdigest())
    (output/'identity.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2))

if __name__=='__main__': main()
