#!/usr/bin/env python3
"""Select the GNU host library closure; does not copy or mix OHOS libraries."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--runtime',type=Path,required=True);p.add_argument('--package',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--steam',action='store_true',help='Include dependencies of the existing Linux ARM64 Steam SDK')
a=p.parse_args();runtime=a.runtime.resolve(strict=True);package=a.package.resolve(strict=True);a.output.mkdir(parents=True,exist_ok=True)
manifest=json.loads((runtime/'manifest.json').read_text())
seeds=['steamohos_shell','Xwayland','libsteamohos_xkbcomp.so','libsteamohos_software_gl.so','libasound.so.2','libasound_module_pcm_hmosdeck.so','libudev.so.1','libXi.so.6','libXrandr.so.2','libXcursor.so.1','libXinerama.so.1','libXcomposite.so.1','libXrender.so.1','libXfixes.so.3',
       # bcrypt/secur32 load GnuTLS dynamically; DT_NEEDED alone misses it.
       'libgnutls.so.30']
queue=[runtime/n for n in seeds]+[package/('files/lib/wine/aarch64-unix/'+n+'.so') for n in ['winex11','winealsa','winebus','opengl32']]
if a.steam:queue.append(runtime/'steamrtarm64/steamclient.so')
selected={};visited=set()
while queue:
 path=queue.pop();name=path.name
 if str(path) in visited:continue
 visited.add(str(path))
 text=subprocess.check_output(['readelf','-d',str(path)],text=True)
 # Steam SDK dependencies also live under steamrtarm64. Visit and stage
 # that entire dependency closure, not only libraries in the runtime root.
 # The SDK itself is referenced separately at HOME/.steam/sdkarm64.
 if path.is_relative_to(runtime) and name not in {"libGL.so.1"} and path!=runtime/'steamrtarm64/steamclient.so':
  data=path.read_bytes();assert data[:4]==b'\x7fELF' and int.from_bytes(data[18:20],'little')==183
  relative=str(path.relative_to(runtime));digest=hashlib.sha256(data).hexdigest()
  if relative in manifest['files']:assert digest==manifest['files'][relative]['sha256']
  if name in selected:assert selected[name]['sha256']==digest, 'Conflicting GNU dependency '+name
  selected[name]={'source':relative,'target':name,'bytes':len(data),'sha256':digest}
 for n in re.findall(r'\(NEEDED\).*?\[(.*?)\]',text):
  target=runtime/n
  if not target.is_file():target=runtime/'steamrtarm64'/n
  if not target.is_file():target=package/'files/lib/wine/aarch64-unix'/n
  if not target.is_file():raise ValueError('Missing GNU dependency '+n)
  queue.append(target)
rows=sorted(selected.values(),key=lambda x:x['target'])
(a.output/'runtime-modules.list').write_text(''.join(x['target']+'\n' for x in rows))
(a.output/'runtime-module-closure.json').write_text(json.dumps({'runtimeManifestSha256':hashlib.sha256((runtime/'manifest.json').read_bytes()).hexdigest(),'modules':rows,'selectedBytes':sum(x['bytes'] for x in rows),'graphicsAlias':{'source':'libsteamohos_software_gl.so','target':'libGL.so.1'}},indent=2)+'\n')
print('Selected',len(rows),'verified GNU runtime files,',sum(x['bytes'] for x in rows),'bytes')
