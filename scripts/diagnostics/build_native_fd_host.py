from pathlib import Path
import subprocess,json,shlex,hashlib
repo=Path('/data/src/winehua');source=repo/'workspace_temp/stardew-context-fix-20261017/source'
build=repo/'workspace_temp/main-proton-migration-implementation-20261016/virgl-build'
out=repo/'workspace_temp/linux-proton'
out.mkdir(parents=True,exist_ok=True)
subprocess.run(['bash',str(repo/'scripts/apply_virglrenderer_ohos_patches.sh'),str(source)],check=True)
rows=json.loads((build/'compile_commands.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs={'patch':sha(repo/'patches/virglrenderer/0007-ohos-native-memory-context.patch'),
        'resourcePatch':sha(repo/'patches/virglrenderer/0008-ohos-native-resource-contract.patch'),
        'cleanupPatch':sha(repo/'patches/virglrenderer/0009-device-object-cleanup.patch'),
        'fdPatch':sha(repo/'patches/virglrenderer/0010-render-resource-fd-close.patch'),
        'script':sha(Path(__file__)), 'archive':sha(build/'src/libvirgl.a'),
        'vtestArchive':sha(build/'vtest/libvtest.a'),
        'compileCommands':sha(build/'compile_commands.json')}
# Cached-source edits and header changes must invalidate a diagnostic build.
# A patch hash alone does not describe the actual compiler inputs.
inputs['source']={str(p.relative_to(source)):sha(p) for directory in
                 ['src/venus','src/proxy','vtest','server']
                 for p in sorted((source/directory).glob('*'))
                 if p.is_file() and p.suffix in ['.c','.h']}
record=out/'native-fd-host-build.json'
if record.exists():
 old=json.loads(record.read_text())
 if old.get('inputs')==inputs and all((out/f).exists() and sha(out/f)==s for f,s in old['outputs'].items()):
  print('Reused native fd receiver candidate');raise SystemExit(0)
def run(c):
 r=subprocess.run(c,cwd=build,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
 if r.returncode:print(r.stdout[-12000:]);raise SystemExit(r.returncode)
 return r.stdout
objects={}
for name in ['vkr_device.c','vkr_device_memory.c','vkr_physical_device.c','vkr_renderer.c','vkr_context.c',
             'vkr_buffer.c','vkr_image.c','virglrenderer.c','vtest_renderer.c','proxy_context.c','render_context.c']:
 row=next(x for x in rows if Path(x['file']).name==name)
 cmd=shlex.split(row['command']);oldsrc=cmd[cmd.index('-c')+1]
 src=source/Path(oldsrc).relative_to('../virgl-color')
 obj=out/Path(row['output']).name
 cmd=[x.replace('../virgl-color',str(source)) for x in cmd]
 cmd[cmd.index('-c')+1]=str(src)
 for flag in ['-o','-MF','-MQ']:
  cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
 print('Compiling',name,flush=True);run(cmd);objects[name]=obj
cc=Path(cmd[0]);ar=cc.with_name('llvm-ar');strip=cc.with_name('llvm-strip')
archive=out/'native-fd-libvirgl.a'
if archive.exists():archive.unlink()
subprocess.run([str(ar),'-M'],input=f'CREATE {archive}\nADDLIB {build}/src/libvirgl.a\nSAVE\nEND\n',text=True,check=True)
run([str(ar),'r',str(archive),*[str(p) for n,p in objects.items() if n not in ['virglrenderer.c','vtest_renderer.c']]])
vtest=out/'native-fd-libvtest.a'
if vtest.exists():vtest.unlink()
subprocess.run([str(ar),'-M'],input=f'CREATE {vtest}\nADDLIB {build}/vtest/libvtest.a\nSAVE\nEND\n',text=True,check=True)
run([str(ar),'r',str(vtest),str(objects['vtest_renderer.c'])])
for target,name in [('src/libvirglrenderer.so.1.11.0','native-fd-libvirglrenderer.so.1'),('vtest/libwinehua_vtest_server.so','native-fd-libwinehua_vtest_server.so')]:
 cmd=shlex.split(run(['ninja','-t','commands',target]).splitlines()[-1]);cmd[cmd.index('-o')+1]=str(out/name)
 for i,x in enumerate(cmd):
  if x=='src/libvirgl.a':cmd[i]=str(archive)
  elif x=='vtest/libvtest.a':cmd[i]=str(vtest)
  elif x=='src/libvirglrenderer.so.1.11.0' and target.startswith('vtest/'):cmd[i]=str(out/'native-fd-libvirglrenderer.so.1')
  elif x.endswith('/virglrenderer.c.o'):cmd[i]=str(objects['virglrenderer.c'])
 cmd+=['-lnative_buffer','-lnative_window'];run(cmd);run([str(strip),'--strip-unneeded',str(out/name)])
 print(name,(out/name).stat().st_size,sha(out/name),flush=True)
record.write_text(json.dumps({'inputs':inputs,'outputs':{f:sha(out/f) for f in ['native-fd-libvirglrenderer.so.1','native-fd-libwinehua_vtest_server.so']},'defaultEnabled':False},indent=2)+'\n')
