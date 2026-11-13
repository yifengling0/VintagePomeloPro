#!/usr/bin/env python3
"""Package a full, paired Wine component candidate; preserve independent runtimes.

The verified FEX test HAP is the base. This is deliberately separate from the
production source-build gate and never promotes the production component pins.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from zipfile import ZipFile, ZIP_DEFLATED

from prepare_fex_upstream import ROOT
from package_fex_upstream_candidate import file_sha, stream_sha, encoded, PAYLOAD, MANIFEST, VERSION
from stamp_wine_builtin_identity import is_builtin, stamp, validate_archive as validate_stamp
from runtime_library_guard import validate_library, validate_thread_exit_library
from source_build_contract import elf_identity
from check_runtime_components import elf_needed
from wine_gecko import check_wine_version, load_lock, validate_payload as validate_gecko
from icu_compat_runtime import validate as validate_icu
from fex_simd_runtime import validate as validate_fex
from wined3d_runtime_contract import source_contract, validate_runtime as validate_wined3d

LOCK=ROOT/'docs/upstream-sync/wine-candidate.lock.json'
RECORD='wine-upstream-candidate.json'
EXTENSIONS={'.dll','.drv','.exe','.sys','.acm','.ax','.ocx','.tlb','.msstyles'}


def mapped_outputs(build):
    result={}
    for group in ('dlls','programs'):
        for arch in ('aarch64-windows','arm64ec-windows','i386-windows'):
            for path in sorted((build/group).glob('*/'+arch+'/*')):
                if not path.is_file() or path.suffix not in EXTENSIONS:
                    continue
                if group=='programs' and (re.match(r'winehua_.*_smoke\.exe$',path.name) or path.name=='winehua_dinput_probe.exe'):
                    continue
                slot='i386-windows' if arch=='i386-windows' else 'aarch64-windows'
                name='bin/'+(slot+'/' if group=='dlls' or slot=='i386-windows' else '')+path.name
                if name in result:
                    raise ValueError('Ambiguous Wine output: '+name)
                result[name]=path
    result.update({'libs/arm64-v8a/'+p.name:p for p in sorted((build/'dlls').glob('*/*.so'))})
    for pattern,folder in (('nls/*.nls','nls'),('include/*.winmd','winmd')):
        for path in sorted(build.glob(pattern)):
            # Make creates links using the container's absolute source path.
            # Resolve that mount explicitly when auditing from the WSL host.
            target=path
            if path.is_symlink():
                link=path.readlink()
                mount=Path('/data/src/winehua')
                target=ROOT/link.relative_to(mount) if link.is_absolute() and link.is_relative_to(mount) else path.resolve()
                if not target.resolve().is_relative_to(ROOT):raise ValueError('External Wine data symlink')
            result['share/wine/'+folder+'/'+path.name]=target
    result['share/wine/wine.inf']=build/'loader/wine.inf'
    return result


def write_zip(before,output,changes,removed):
    with ZipFile(output,'w',compression=ZIP_DEFLATED,allowZip64=True) as after:
        for item in before.infolist():
            if item.filename in removed:
                continue
            if item.filename in changes:
                value=changes[item.filename]
                if isinstance(value,bytes):after.writestr(copy.copy(item),value)
                else:
                    with value.open('rb') as src,after.open(copy.copy(item),'w',force_zip64=True) as dest:shutil.copyfileobj(src,dest,1024*1024)
            else:
                with before.open(item) as src,after.open(copy.copy(item),'w',force_zip64=True) as dest:shutil.copyfileobj(src,dest,1024*1024)
        for name in sorted(set(changes)-set(before.namelist())):
            value=changes[name]
            if isinstance(value,bytes):after.writestr(name,value)
            else:after.write(value,name)


def audit(before,after,changes,removed):
    expected=(set(before.namelist())-set(removed))|set(changes)
    if len(after.namelist())!=len(expected) or set(after.namelist())!=expected:
        raise ValueError('Wine candidate ZIP inventory mismatch')
    changed=[]
    for name in sorted(expected):
        with after.open(name) as f:actual=stream_sha(f)
        if name in changes:
            data=changes[name]
            checksum=hashlib.sha256(data).hexdigest() if isinstance(data,bytes) else file_sha(data)
            if actual!=checksum:raise ValueError('Packaged component differs: '+name)
        if name in before.namelist():
            with before.open(name) as f:old=stream_sha(f)
            if old!=actual:
                if name not in changes:raise ValueError('Unrelated member changed: '+name)
                changed.append(name)
    return changed


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--base-receipt',type=Path,required=True)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--container',required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    for path in (a.build,a.output.parent):
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT/'workspace_temp'):
            raise ValueError('Use the isolated candidate task directory')
    if a.output.exists() or a.output.resolve()==a.baseline.resolve():raise ValueError('Inspect/reuse existing candidate')
    base=json.loads(a.base_receipt.read_text());lock=json.loads(LOCK.read_text())
    if a.baseline.stat().st_size!=base['signedBytes'] or file_sha(a.baseline)!=base['signedSha256']:
        raise ValueError('FEX base HAP differs from signed receipt')
    buildpath=a.build/'wine-full-build.json';build=json.loads(buildpath.read_text())
    if build['stage']!='full' or build['adaptedTree']!=lock['adaptedTree'] or build['adaptedCommit']!=lock['adaptedCommit']:
        raise ValueError('A full build from the locked candidate is required')
    if (a.build/'inputs-pending.json').exists():raise ValueError('Candidate build is incomplete')
    actual=subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD']).decode().strip()
    if actual!=lock['adaptedCommit'] or subprocess.check_output(['git','-C',str(a.source),'status','--porcelain']).strip():
        raise ValueError('Wine source differs from build lock')
    check_wine_version(a.source,load_lock())
    for name,item in build['outputs'].items():
        f=a.build/name
        if f.stat().st_size!=item['bytes'] or file_sha(f)!=item['sha256']:raise ValueError('Changed build output: '+name)
    package=a.build/'package';package.mkdir(exist_ok=True)
    old=a.build/'package-base.zip';new=package/'runtime.zip'
    override=dict(schemaVersion=1,policy='component-test-candidate',productionPinsChanged=False,deviceValidated=False,
        baseSourceCommit=base['sourceCommit'],
        upstreamCommit=lock['upstream']['commit'],adaptedCommit=lock['adaptedCommit'],adaptedTree=lock['adaptedTree'],
        lockSha256=file_sha(LOCK),buildReceiptSha256=file_sha(buildpath),baseSignedSha256=base['signedSha256'],
        independentComponentsPreserved=['FEX','ICU','Gecko x86/x64','DXVK','VKD3D','GStreamer dependencies','fonts and user settings'])
    with ZipFile(a.baseline) as hap:
        manifest=json.loads(hap.read(MANIFEST))
        if not old.exists():
            with hap.open(PAYLOAD) as src,old.open('wb') as dest:shutil.copyfileobj(src,dest,1024*1024)
        if file_sha(old)!=manifest['payloadSha256']:raise ValueError('Base runtime checksum differs')
        outputs=mapped_outputs(a.build/'wine-ohos-aarch64')
        outputs['libs/arm64-v8a/libwineserver.so']=a.build/'wine_server-aarch64/libwineserver.so'
        for path in sorted((a.source/'fonts').glob('*.ttf')):outputs['share/wine/fonts/'+path.name]=path
        inventory_path=ROOT/lock['baseRuntimeInventory']['path']
        if file_sha(inventory_path)!=lock['baseRuntimeInventory']['sha256']:raise ValueError('Baseline output inventory changed')
        previous=json.loads(inventory_path.read_text())
        with ZipFile(old) as runtime:
            icu=json.loads(runtime.read('icu-compat.json'));preserved=set(icu['files'])
            # The standalone ICU build is a separately pinned, typed ARM64EC component.
            outputs={n:f for n,f in outputs.items() if n not in preserved}
            known=set(previous['members'])
            removed=(known-set(outputs)-preserved)&set(runtime.namelist())
            removed_native={n for n in removed if n.startswith('libs/')}
            removed-=removed_native
            native={};changes={};provenance={}
            strip='/apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-strip'
            strip_commands=[]
            for name,path in outputs.items():
                dest=package/'staged'/name;dest.parent.mkdir(parents=True,exist_ok=True)
                if path.suffix=='.so' or path.suffix in EXTENSIONS- {'.tlb','.msstyles'}:
                    # Never strip the compiler cache or a shared canonical artifact in place.
                    command=[strip,'--strip-debug','-o','/data/src/winehua/'+str(dest.relative_to(ROOT)),
                             '/data/src/winehua/'+str(path.relative_to(ROOT))]
                    strip_commands.append(command)
                else:shutil.copyfile(path,dest)
            runner='import json,subprocess,sys\nfor command in json.load(sys.stdin):subprocess.run(command,check=True,timeout=30)\n'
            subprocess.run(['docker','exec','-i',a.container,'python3','-c',runner],input=json.dumps(strip_commands).encode(),capture_output=True,check=True,timeout=180)
            for name,path in outputs.items():
                dest=package/'staged'/name
                if name.startswith('libs/'):
                    data=dest.read_bytes();validate_library(data,name)
                    if data[:6]!=b'\x7fELF\x02\x01' or data[18:20]!=b'\xb7\x00':raise ValueError('Wrong Unix architecture: '+name)
                    native[name]=dest
                else:changes[name]=dest
                provenance[name]=dict(buildPath=str(path.relative_to(ROOT)),buildSha256=file_sha(path),sha256=file_sha(dest),bytes=dest.stat().st_size)
            # Apply exactly the product font substitution block to the newly generated INF.
            assembly=(ROOT/'scripts/assemble.sh').read_text()
            block=assembly.split("sed_i '/^\\[MCI\\]$/i\\\n",1)[1].split("' \"$wine_data/share/wine/wine.inf\"",1)[0].replace('\\\n','\n')
            inf=changes['share/wine/wine.inf'].read_bytes()
            if inf.count(b'[MCI]')!=1:raise ValueError('Wine INF layout changed')
            inf=inf.replace(b'[MCI]',block.encode()+b'\n[MCI]')
            def read(name):
                value=changes.get(name)
                return value if isinstance(value,bytes) else value.read_bytes() if value is not None else runtime.read(name)
            names=(set(runtime.namelist())-removed)|set(changes)
            changes['share/wine/wine.inf']=stamp(inf,[(n,hashlib.sha256(read(n)).hexdigest()) for n in names if is_builtin(n)])
            graphics={'version':1,**source_contract(a.source),'files':[
                dict(path='bin/'+arch+'/wined3d.dll',bytes=len(read('bin/'+arch+'/wined3d.dll')),
                     sha256=hashlib.sha256(read('bin/'+arch+'/wined3d.dll')).hexdigest()) for arch in ('i386-windows','aarch64-windows')]}
            validate_wined3d(graphics,read);changes['wined3d-graphics.json']=encoded(graphics)
            manifest['wineD3DGraphics']=graphics
            manifest['commandQueueWake']['files']=graphics['files']
            ntdll=native['libs/arm64-v8a/ntdll.so'].read_bytes();validate_thread_exit_library(ntdll,'ntdll.so')
            if 'threadExit' in manifest:manifest['threadExit']['sha256']=hashlib.sha256(ntdll).hexdigest()
            receipt=json.loads(runtime.read('source-build-candidate.json'))
            receipt['componentOverrides']['wine']=override
            for name in removed:receipt['payload'].pop(name,None)
            for name,path in native.items():
                data=path.read_bytes();receipt['libraries'][name]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),loadableSha256=elf_identity(data))
            for name in removed_native:receipt['libraries'].pop(name,None)
            changes[RECORD]=encoded(override)
            for name,value in changes.items():
                data=value if isinstance(value,bytes) else value.read_bytes()
                receipt['payload'][name]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
            changes['source-build-candidate.json']=encoded(receipt)
            write_zip(runtime,new,changes,removed)
            with ZipFile(new) as result:
                inner=audit(runtime,result,changes,removed)
                validate_icu(icu,result.read);validate_fex(json.loads(result.read('fex-simd-zero.json')),result.read)
                components=json.loads(result.read('runtime-components.json'))
                validate_gecko(result.namelist(),result.read,components['gecko'])
                validate_stamp(result)
        manifest['payloadSha256']=file_sha(new);manifest['componentOverrides']['wine']=override
        version=hap.read(VERSION).decode().strip()
        version=re.sub(r'content=[^;\s]+','content='+file_sha(new)[:16],version)
        version=re.sub(r';wined3d-graphics=[^;\s]+',';wined3d-graphics='+hashlib.sha256(encoded(graphics)).hexdigest()[:16],version)
        if 'threadExit' in manifest:version=re.sub(r';thread-exit=[^;\s]+',';thread-exit='+manifest['threadExit']['sha256'][:16],version)
        outer={**native,PAYLOAD:new,MANIFEST:encoded(manifest),VERSION:(version+';wine-candidate='+lock['adaptedCommit'][:10]+'\n').encode()}
        # Every native dependency must be supplied by this package or the platform.
        available=(set(hap.namelist())-removed_native)|set(outer)
        for name,path in native.items():
            for needed in elf_needed(name,path.read_bytes()):
                if needed not in ('libc.so','libm.so','libdl.so','libpthread.so','ld-musl-aarch64.so.1') and 'libs/arm64-v8a/'+needed not in available:
                    raise ValueError('Missing native dependency: '+name+' -> '+needed)
        write_zip(hap,a.output,outer,removed_native)
        with ZipFile(a.output) as result:outer_changed=audit(hap,result,outer,removed_native)
    report=dict(override,candidate=str(a.output),unsignedSha256=file_sha(a.output),unsignedBytes=a.output.stat().st_size,
        changedHapMembers=outer_changed,changedRuntimeMembers=inner,removedRuntimeMembers=sorted(removed),
        removedHapMembers=sorted(removed_native),packagedOutputs=provenance,unchangedMembersSha256Verified=True)
    a.output.with_suffix('.json').write_bytes(encoded(report))
    print(json.dumps({k:v for k,v in report.items() if k not in ('packagedOutputs','changedRuntimeMembers')}))


if __name__=='__main__':main()
