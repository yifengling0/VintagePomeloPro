#!/usr/bin/env python3
"""Build opt-in native BC shaders/chain metadata without replacing release files."""
import argparse, hashlib, json, re, struct, subprocess
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/native_bc'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--glslang',type=Path,default=Path('/usr/bin/glslangValidator'))
    p.add_argument('--registry',type=Path,default=ROOT/'thirdparty/mesa/src/vulkan/registry/vk.xml')
    p.add_argument('--headers',type=Path,default=ROOT/'thirdparty/mesa/include/vulkan')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    inputs={str(x.relative_to(ROOT)) if x.is_relative_to(ROOT) else str(x):sha(x) for x in
        [Path(__file__).resolve(),SOURCE/'transcode.comp',SOURCE/'decode.glsl',a.registry.resolve(),a.glslang.resolve()]}
    inputs['glslangVersion']=subprocess.check_output([str(a.glslang.resolve()),'--version'],text=True,timeout=15)
    inputs['headers']={x.name:sha(x) for x in sorted(a.headers.glob('*.h'))}
    declarations='\n'.join(x.read_text() for x in a.headers.glob('*.h') if x.name!='vulkan_sc_core.h')
    manifest=out/'native-bc-shaders.json';spv=out/'native_bc.spv';header=out/'native_bc_spirv.h';sizes=out/'native_bc_sizes.h'
    old=json.loads(manifest.read_text()) if manifest.exists() else {}
    if old.get('inputs')==inputs and all(x.exists() and old.get('outputs',{}).get(x.name)==sha(x) for x in [spv,header,sizes]):
        print('Reused native BC shader/metadata');return
    subprocess.run([str(a.glslang.resolve()),'-V','--target-env','vulkan1.1','-I'+str(SOURCE),
        '-o',str(spv),str(SOURCE/'transcode.comp')],check=True,timeout=60)
    code=spv.read_bytes();assert len(code)%4==0 and len(code)<2*1024*1024
    words=struct.unpack('<'+'I'*(len(code)//4),code)
    header.write_text('static const uint32_t native_bc_spirv[]={'+','.join(hex(x) for x in words)+'};\n')
    registry=ET.parse(a.registry).getroot();protected={}; excluded=set()
    platforms={x.get('name'):x.get('protect') for x in registry.findall('./platforms/platform')}
    for ext in registry.findall('./extensions/extension'):
        if 'vulkan' not in (ext.get('supported') or '').split(','):
            excluded.update(t.get('name') for t in ext.findall('./require/type'))
            continue
        protect=platforms.get(ext.get('platform')) or ('VK_ENABLE_BETA_EXTENSIONS' if ext.get('provisional')=='true' else None)
        for t in ext.findall('./require/type'):
            if protect:protected[t.get('name')]=protect
    lines=['// Generated sizes for the device-feature and memory-allocation pNext chains.',
        'static size_t native_bc_struct_size(VkStructureType type){switch(type){']
    for t in registry.findall('./types/type'):
        extends=set((t.get('structextends') or '').split(','))
        if not extends.intersection({'VkDeviceCreateInfo','VkMemoryAllocateInfo',
                'VkPhysicalDeviceImageFormatInfo2','VkImageCreateInfo'}):continue
        member=next((m for m in t.findall('member') if m.findtext('name')=='sType'),None)
        name=t.get('name')
        if member is None or not member.get('values') or not name or name in excluded:continue
        if not re.search(r'\bstruct\s+'+re.escape(name)+r'\s*\{',declarations):continue
        if t.get('api') and 'vulkan' not in t.get('api').split(','):continue
        protect=protected.get(name)
        if protect:lines.append('#ifdef '+protect)
        lines.append('case '+member.get('values')+':return sizeof('+name+');')
        if protect:lines.append('#endif')
    lines.append('default:return 0;}}')
    sizes.write_text('\n'.join(lines)+'\n')
    manifest.write_text(json.dumps(dict(inputs=inputs,outputs={x.name:sha(x) for x in [spv,header,sizes]},spirvBytes=len(code)),indent=2)+'\n')
    print('Built native BC shader:',len(code),'bytes')
if __name__=='__main__':main()
