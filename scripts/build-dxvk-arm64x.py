#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build unchanged WineHua DXVK forks with VPP's native ARM64X source/link profile.

Profiles adapted from VintagePomeloPro feature/main_proton@e08ca925d3653fffa5d7e9ffd61bd5a1ea5ca38d:
scripts/build_dxvk_arm64x.sh (53b3250569a2b5288c57d6254d4eb8dc54dbc9e17b5b42b4680823ccfd43d592)
scripts/build_dxvk_modern_arm64x.sh (992e30b74f3d77c37ab4597d4c1d5b38dd799406c019af7c4ba73021363df907).
Only HSL path/provenance/cache/error/export-parser plumbing differs. No fork source edits.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
COMMITS={'legacy-1.10.3':'7c5cc47f772b9564af177cea920da81a40ffd47c','modern-2.6.2':'05a0a66d74cc41e6a90a8cb62d459c1c6a28c783'}
ORDINALS={'dxgi':{'CreateDXGIFactory':9,'CreateDXGIFactory1':10,'CreateDXGIFactory2':11,'DXGIDeclareAdapterRemovalSupport':16,'DXGIGetDebugInterface1':17},'d3d11':{'D3D11CoreCreateDevice':18,'D3D11CreateDevice':22,'D3D11CreateDeviceAndSwapChain':23,'D3D11On12CreateDevice':24}}
LIBRARIES=('ole32','oleaut32','dwmapi','winmm','imm32','comdlg32','uxtheme','setupapi','shlwapi','ws2_32','iphlpapi','gdi32')
def sha(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):h.update(b)
 return h.hexdigest()
def canonical(value):return (json.dumps(value,sort_keys=True,separators=(',',':'))+'\n').encode()
def tree(path):
 result={}
 for f in sorted(path.rglob('*')):
  if '.git' in f.relative_to(path).parts:continue
  if f.is_symlink():raise ValueError('Source symlink not allowed: '+str(f))
  if f.is_file():result[f.relative_to(path).as_posix()]=sha(f)
 return result
def exports(text,module):
 rows={};started=False
 for raw in text.splitlines():
  line=raw.split(';',1)[0].strip()
  if not line:continue
  if line.upper().startswith('LIBRARY '):continue
  if line.upper()=='EXPORTS':started=True;continue
  match=re.fullmatch(r'([A-Za-z_][A-Za-z0-9_]*)\s*@\s*([0-9]+)',line)
  if not started or not match:raise ValueError('Unrecognized .def export: '+raw)
  name,number=match[1],int(match[2])
  if name in rows or number in rows.values() or not 1<=number<=65535:raise ValueError('Duplicate/invalid ordinal')
  rows[name]=number
 if rows!=ORDINALS[module]:raise ValueError('Unexpected complete export/ordinal contract: '+module)
 return rows
def run(command,log,*,timeout=600):
 with Path(log).open('w') as stream:
  p=subprocess.run([str(x) for x in command],stdout=stream,stderr=subprocess.STDOUT,timeout=timeout)
 if p.returncode:raise RuntimeError('Command failed '+str(p.returncode)+'; see '+str(log))
def version(binary):
 if Path(binary).name=='llvm-dlltool':return 'Version not exposed; identity bound by executable SHA and pinned tool archive'
 return subprocess.check_output([str(binary),'--version'],text=True,timeout=20).splitlines()[0]
def generate_shader(glslang,source,target,log):
 # Cached generated headers are never trusted merely because they exist.
 # Keep the prior header intact if the real compiler fails.
 partial=target.with_suffix(target.suffix+'.partial')
 try:
  run([glslang,'--quiet','-V','--vn',source.stem,source,'-o',partial],log)
  if not partial.is_file() or partial.is_symlink():raise ValueError('Missing/unsafe generated shader header')
  os.replace(partial,target)
 finally:partial.unlink(missing_ok=True)
def reusable_object(output,receipt,source,generated_digest):
 if not output.is_file() or output.is_symlink() or not receipt.is_file() or receipt.is_symlink():return False
 try:
  prior=json.loads(receipt.read_text())
  return prior.get('sha256')==sha(output) and prior.get('sourceSha256')==sha(source) and prior.get('generatedInputsSha256')==generated_digest
 except (OSError,ValueError,TypeError):return False
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--variant',choices=tuple(COMMITS),required=True);p.add_argument('--source',type=Path,required=True)
 p.add_argument('--source-inputs',type=Path,required=True);p.add_argument('--mingw',type=Path,required=True);p.add_argument('--glslang',type=Path,required=True)
 p.add_argument('--lock',type=Path,default=ROOT/'docs/linux-steam/gnu-dxvk-source.lock.json');p.add_argument('--reuse-objects',type=Path,help='Reuse only per-object SHA verified objects with byte-identical compilation inputs')
 p.add_argument('--cache',type=Path,default=ROOT/'build/gnu-dxvk/build');p.add_argument('--output',type=Path,default=ROOT/'build/gnu-dxvk/results');p.add_argument('--jobs',type=int,default=4)
 a=p.parse_args()
 if not 1<=a.jobs<=8:p.error('jobs must be 1..8')
 src=a.source.resolve(strict=True);mgw=a.mingw.resolve(strict=True);glslang=a.glslang.resolve(strict=True);modern=a.variant.startswith('modern')
 bcdec=src.parent/'wine/dlls/d3dx9_36/bcdec.h'
 if sha(bcdec)!='ebfff36dd34ef7e9de508800cc24f7e2f78ffd77a958798244626eb9e34f67cf':raise ValueError('Wrong Wine BC decoder header')
 inputs_document=json.loads(a.source_inputs.read_text());files=tree(src)
 # The acquisition receipt carries a complete materialized tree, including pinned gitlinks.
 key='modern' if modern else 'legacy'
 expected=next(row for row in inputs_document['sourceInputs'] if row['name']==key)
 expected_files={row['path']:row['sha256'] for row in inputs_document['sourceTrees'][key]['files']}
 if expected['commit']!=COMMITS[a.variant] or files!=expected_files:raise ValueError('Source tree differs from pinned acquisition receipt')
 locked=json.loads(a.lock.read_text());locked_source=locked['variants'][a.variant]['source']
 if locked.get('schemaVersion')!=2 or locked_source['commit']!=COMMITS[a.variant] or locked_source['materializedTreeSha256']!=hashlib.sha256(canonical(files)).hexdigest():raise ValueError('Source tree not pinned by current source lock')
 if sha(glslang)!=locked['buildInputs']['hostBinaries']['glslangValidator']['sha256']:raise ValueError('Unpinned shader compiler')
 binaries={n:mgw/'bin'/n for n in ('clang','ld.lld','llvm-ar','llvm-dlltool','llvm-readobj')};libroot=mgw/'aarch64-w64-mingw32/lib'
 libraries={n:libroot/n for n in ('libc++abi.a','libunwind.a','libc++.a')}
 archive_listing=subprocess.check_output([binaries['llvm-ar'],'t',libraries['libc++.a']],text=True,timeout=30)
 if 'obj.arm64ec/' not in archive_listing:raise ValueError('Toolchain C++ archive lacks EC graph')
 compiler_version=version(binaries['clang'])
 if not compiler_version.startswith('clang version 23.1.0'):raise ValueError('Pinned llvm-mingw 20260826 clang23.1.0 required')
 includes=[src/'src',src/'include']
 includes+=([src/'include/vulkan/include',src/'include/spirv/include',src/'include/native',src/'subprojects/libdisplay-info/include'] if modern else [src/'include/spirv',src/'include/vulkan'])
 base=['-O2','-DNDEBUG','-DNOMINMAX','-D_WIN32_WINNT=0xa00']
 cpp=['-std=c++17']+base+(['-DDXVK_WSI_WIN32'] if modern else [])
 c=['-std=c11']+base+(['-D_POSIX_C_SOURCE=200809L','-Dstatic_array=static'] if modern else [])
 inp={'schema':1,'variant':a.variant,'sourceCommit':COMMITS[a.variant],'sourceFiles':files,'bcdecSha256':sha(bcdec),'sourceInputsSha256':sha(a.source_inputs),'builderSha256':sha(Path(__file__)),
      'toolchain':{n:{'sha256':sha(f),'version':version(f)} for n,f in binaries.items()},'staticLibraries':{n:sha(f) for n,f in libraries.items()},'glslang':{'sha256':sha(glslang),'version':version(glslang)},'target':['--target=aarch64-w64-mingw32','-marm64x'],'cppFlags':cpp,'cFlags':c,'libraries':LIBRARIES,'exportOrdinals':ORDINALS,'vulkanImportBoundary':'llvm-dlltool -m arm64ec -d public-vulkan.def -N public-vulkan.def; no Wine Unix objects'}
 for name,path in binaries.items():
  if sha(path)!=locked['buildInputs']['llvmMingw']['binarySha256'][name]:raise ValueError('Unpinned LLVM tool: '+name)
 for name,path in libraries.items():
  if sha(path)!=locked['buildInputs']['llvmMingw']['staticLibrarySha256'][name]:raise ValueError('Unpinned static runtime: '+name)
 reuse=None
 if a.reuse_objects:
  prior=json.loads((a.reuse_objects/'inputs.json').read_text())
  compile_keys=('sourceCommit','sourceFiles','bcdecSha256','sourceInputsSha256','toolchain','staticLibraries','glslang','target','cppFlags','cFlags')
  if any(prior.get(k)!=inp[k] for k in compile_keys):raise ValueError('Object reuse compilation inputs differ')
  reuse=a.reuse_objects/'obj'
 inp['linkProfileCorrections']=['ordinal export shim before dllexport objects','modern d3d11 includes dxgi_format object as declared by VPP comment']
 fingerprint=hashlib.sha256(canonical(inp)).hexdigest();cache=a.cache/a.variant/fingerprint;gen=cache/'gen';obj=cache/'obj';logs=cache/'logs';out=a.output/a.variant/'arm64x';bindir=out/'bin'
 for d in (gen,obj,logs,bindir):d.mkdir(parents=True,exist_ok=True)
 (cache/'inputs.json').write_bytes(canonical(inp));(gen/'version.h').write_text((src/'version.h.in').read_text().replace('@VCS_TAG@','v2.6' if modern else 'v1.10.3'));generated_names={'version.h'}
 if modern:
  (gen/'buildenv.h').write_text('#pragma once\n#define DXVK_TARGET "aarch64"\n#define DXVK_COMPILER "clang"\n#define DXVK_COMPILER_VERSION "23"\n');generated_names.add('buildenv.h')
 shaders=[]
 for folder in ('src/dxvk/shaders','src/dxvk/hud/shaders','src/d3d11/shaders')+(() if modern else ('src/d3d9/shaders',)):
  shaders += sorted(f for f in (src/folder).rglob('*') if f.suffix in ('.vert','.frag','.comp','.geom'))
 names=set()
 for f in shaders:
  if f.stem in names:raise ValueError('Shader basename collision')
  names.add(f.stem);target=gen/(f.stem+'.h')
  generate_shader(glslang,f,target,logs/(f.stem+'.shader.log'));generated_names.add(target.name)
 if modern:
  run(['python3',src/'subprojects/libdisplay-info/tool/gen-search-table.py',src/'subprojects/libdisplay-info/pnp.ids',gen/'pnp-id-table.c','pnp_id_table'],logs/'pnp-table.log');generated_names.add('pnp-id-table.c')
 # Only declared compile inputs, not later link-only .def/.S outputs.
 generated_files={name:sha(gen/name) for name in sorted(generated_names)}
 generated_digest=hashlib.sha256(canonical(generated_files)).hexdigest()
 folders=('util','spirv','vulkan','dxvk','dxbc','dxgi','d3d10','d3d11')+(('wsi',) if modern else ('d3d9','dxso'))
 sources=[]
 for folder in folders:
  for f in sorted((src/'src'/folder).rglob('*')):
   if f.suffix in ('.cpp','.c'):sources.append((f,f.relative_to(src/'src').as_posix().replace('/','_')+'.o'))
 if modern:
  sources += [(f,'libdisplay-info_'+f.name+'.o') for f in sorted((src/'subprojects/libdisplay-info').glob('*.c'))]
  sources += [(gen/'pnp-id-table.c','pnp-id-table.c.o')]
 if len({n for _,n in sources})!=len(sources):raise ValueError('Object-name collision')
 targetflags=inp['target'];inc=['-I'+str(x) for x in includes+[gen]]
 def compile_one(row):
  source,name=row;output=obj/name;receipt=output.with_suffix('.json');flags=c if source.suffix=='.c' else cpp
  if reusable_object(output,receipt,source,generated_digest):return
  if reuse is not None:
   old=reuse/name;old_receipt=old.with_suffix('.json')
   if reusable_object(old,old_receipt,source,generated_digest):
    previous=json.loads(old_receipt.read_text());temporary=output.with_suffix('.reuse');temporary.unlink(missing_ok=True)
    os.link(old,temporary);os.replace(temporary,output);receipt.write_bytes(canonical(previous));return
  command=[binaries['clang'],*targetflags,*flags,*inc,'-c',source,'-o',str(output)+'.partial'];run(command,logs/(name+'.log'))
  os.replace(str(output)+'.partial',output);receipt.write_bytes(canonical({'sha256':sha(output),'sourceSha256':sha(source),'generatedInputsSha256':generated_digest}))
 print(json.dumps({'stage':'compile','variant':a.variant,'sources':len(sources),'fingerprint':fingerprint}),flush=True)
 with ThreadPoolExecutor(max_workers=a.jobs) as pool:
  futures=[pool.submit(compile_one,row) for row in sources]
  for index,f in enumerate(as_completed(futures),1):
   f.result()
   if index%25==0:print(json.dumps({'stage':'compile','variant':a.variant,'complete':index,'total':len(sources)}),flush=True)
 # A public Windows Vulkan import archive with native and EC import members.
 definition=gen/'vulkan.def';definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n    vkGetInstanceProcAddr\n');vulkan=cache/'libvulkan-1.a'
 run([binaries['llvm-dlltool'],'-m','arm64ec','-d',definition,'-N',definition,'-l',vulkan],logs/'vulkan-import.log')
 archive_info=subprocess.check_output([binaries['llvm-readobj'],'--file-headers',vulkan],text=True,timeout=30)
 if 'COFF-import-file-ARM64EC' not in archive_info or 'COFF-import-file-ARM64\n' not in archive_info:raise ValueError('Public Vulkan import archive is not dual-graph')
 (cache/'vulkan-import.txt').write_text(archive_info);results={}
 stage_manager=tempfile.TemporaryDirectory(prefix='.hsl-native-link-',dir=out);stage=Path(stage_manager.name)
 for module in ('dxgi','d3d11'):
  keep=['util_','spirv_','vulkan_','wsi_','dxvk_']
  if module=='dxgi':keep+=['dxgi_']
  elif modern:keep+=['dxgi_dxgi_format','dxbc_','d3d10_','d3d11_']
  else:keep+=['dxgi_dxgi_format','dxgi_dxgi_monitor','dxgi_dxgi_swapchain','dxbc_','d3d10_','d3d11_']
  objects=[]
  for _,name in sources:
   if not modern and name.startswith('d3d10_d3d10_main'):continue
   if any(name.startswith(prefix) for prefix in keep) or (modern and name.startswith(('libdisplay-info_','pnp-id-table'))):objects.append(obj/name)
  contract=exports((src/f'src/{module}/{module}.def').read_text(),module);shim=gen/(module+'.exports.S')
  shim.write_text('.section .drectve,"r"\n'+''.join('.asciz "/EXPORT:'+name+',@'+str(number)+'"\n' for name,number in contract.items()))
  shimobj=cache/(module+'.exports.o');run([binaries['clang'],*targetflags,'-c',shim,'-o',shimobj],logs/(module+'.exports.log'))
  binary=bindir/(module+'.dll');partial=stage/(module+'.dll');implib=cache/(module+'.lib')
  command=[binaries['clang'],*targetflags,'-shared','-o',partial,'-Wl,--out-implib='+str(implib),shimobj,*objects]
  if module=='d3d11':command+=[cache/'dxgi.lib']
  command += [vulkan,'-L'+str(libroot),'-Wl,--start-group',*libraries.values(),'-Wl,--end-group',*('-l'+n for n in LIBRARIES)]
  print(json.dumps({'stage':'link','variant':a.variant,'module':module,'objects':len(objects)}),flush=True);run(command,logs/(module+'.link.log'))
  if modern and b'DXVK_WSI_DRIVER environment variable unset' in partial.read_bytes():raise ValueError('Modern Win32 WSI not selected')
  inventory=subprocess.check_output([binaries['llvm-readobj'],'--file-headers','--coff-imports','--coff-exports','--coff-load-config',partial],text=True,timeout=60);(out/(module+'.llvm-readobj.txt')).write_text(inventory)
  if re.search(r'Name:.*(?:libc\+\+|libunwind)',inventory,re.I):raise ValueError('Dynamic C++ runtime dependency')
  results[module+'.dll']={'bytes':partial.stat().st_size,'sha256':sha(partial),'expectedExports':contract,'objectCount':len(objects)}
 # Strict inventory helper is authoritative for both PE views; no substring-only compatibility claim.
 helper=ROOT/'scripts/dxvk_pe.py'
 if not helper.exists():raise RuntimeError('Dual-graph inventory helper required before receipt publication')
 helper_sha=sha(helper)
 spec=importlib.util.spec_from_file_location('dxvk_pe',helper);inspector=importlib.util.module_from_spec(spec);spec.loader.exec_module(inspector)
 for name,row in results.items():row['pe']=inspector.validate_dxvk((stage/name).read_bytes(),name,architecture='arm64x')
 if tree(src)!=files or sha(bcdec)!=inp['bcdecSha256'] or sha(Path(__file__))!=inp['builderSha256'] or sha(helper)!=helper_sha or {name:sha(gen/name) for name in sorted(generated_names)}!=generated_files:raise ValueError('Source/build/inventory/generated implementation changed during compilation')
 for name in results:os.replace(stage/name,bindir/name)
 stage_manager.cleanup()
 receipt={'schema':2,'variant':a.variant,'architecture':'arm64x','sourceCommit':COMMITS[a.variant],'sourceTreeSha256':hashlib.sha256(canonical(files)).hexdigest(),'inputsSha256':fingerprint,'inputs':inp,'generatedInputsSha256':generated_digest,'generatedInputs':generated_files,'files':results,'inventoryHelperSha256':sha(helper),'publicVulkanImportSha256':sha(vulkan),'runtimeExecuted':False,'profileScope':'native dxgi+d3d11 only; no native D3D9 or complete native D3D10 profile'}
 (out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({'stage':'complete','variant':a.variant,'receipt':str(out/'build.json'),'files':{k:v['sha256'] for k,v in results.items()}}),flush=True)
if __name__=='__main__':main()
