from pathlib import Path
import argparse,json,subprocess,shlex,hashlib,shutil,os
p=argparse.ArgumentParser(description='Build a private GNU Venus variant with coherent Wine low-address aliases; original Mesa source stays unchanged')
for name in ['source','build','output','meson-tools']:p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();b=a.output.resolve();build=a.build.resolve(strict=True);source=a.source.resolve(strict=True)/'src/virtio/vulkan/vn_renderer_vtest.c'
b.mkdir(parents=True,exist_ok=True)
if b.is_relative_to(source.parents[3]):p.error('Keep generated variant outside shared Mesa sources')
text=source.read_text();needle='const bool keep_wow64_map_fd = false;'
assert text.count(needle)==1
text=text.replace(needle,'const bool keep_wow64_map_fd = true; /* GNU private Wine WSI also creates a coherent low-address alias. */')
text=text.replace('#include "vn_device.h"','#include "vn_device.h"\n#include "vn_device_memory.h"')
text += r'''
/* Private GNU WSI: duplicate only the mapped BO backing descriptor. */
__attribute__((visibility("default"))) int
vn_winehua_mapped_memory_fd(VkDeviceMemory handle, uint64_t *size);
__attribute__((visibility("default"))) int
vn_winehua_mapped_memory_fd(VkDeviceMemory handle, uint64_t *size)
{
   struct vn_device_memory *mem = vn_device_memory_from_handle(handle);
   if (!mem || !mem->base_bo || !mem->mapped || !size) return -1;
   struct vtest_bo *bo = (struct vtest_bo *)mem->base_bo;
   if (bo->res_fd < 0 || !bo->base.mmap_ptr) return -1;
   *size = bo->base.mmap_size;
   return os_dupfd_cloexec(bo->res_fd);
}
'''
target=b/'vn_renderer_vtest.proton.c'
if not target.exists() or target.read_text()!=text:target.write_text(text)
commands=json.loads((build/'compile_commands.json').read_text())
row=next(x for x in commands if x['file'].endswith('/vn_renderer_vtest.c'))
command=shlex.split(row['command']);command[command.index(row['file'])]=str(target)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
recordPath=b/'venus-alias-build.json';out=b/'libvulkan_virtio.so'
inputs={'source':sha(source),'variant':sha(target),'script':sha(Path(__file__)),'command':command}
if recordPath.exists() and out.exists():
 old=json.loads(recordPath.read_text())
 if old.get('inputs')==inputs and old.get('sha256')==sha(out):print('Reused GNU Venus alias build');raise SystemExit(0)
with (b/'venus-alias-build.log').open('w') as log:
 subprocess.run(command,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
 # Link the just-built object directly, so Ninja does not replace it using its original source command.
 env={**os.environ,'PATH':str(a.meson_tools.resolve(strict=True))+':'+os.environ['PATH']}
 steps=subprocess.check_output(['ninja','-C',str(build),'-t','commands','src/virtio/vulkan/libvulkan_virtio.so'],env=env,text=True).splitlines()
 link=shlex.split(steps[-1]);version=Path(link[link.index('-Wl,--version-script')+1]);exports=version.read_text()
 assert exports.count('vn_winehua_present;')==1
 privateExports=b/'vulkan-proton.sym';privateExports.write_text(exports.replace('vn_winehua_present;','vn_winehua_present;\n        vn_winehua_mapped_memory_fd;'))
 link[link.index('-Wl,--version-script')+1]=str(privateExports)
 subprocess.run(link,cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
result=build/'src/virtio/vulkan/libvulkan_virtio.so';out=b/'libvulkan_virtio.so'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
if not out.exists() or sha(out)!=sha(result):shutil.copyfile(result,out)
record={'inputs':inputs,'originalSource':str(source),'originalSha256':sha(source),'isolatedSourceSha256':sha(target),'change':'retain BO backing fd through BO destruction for shared alias; no pixel copying','bytes':out.stat().st_size,'sha256':sha(out)}
(b/'venus-alias-build.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
