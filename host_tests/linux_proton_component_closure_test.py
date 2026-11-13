#!/usr/bin/env python3
"""Regression check for the delivered GNU graphics dependency closure.

Uses the verified incremental graphics build; creates no extracted runtime.
A missing XCB/GLX library must fail before a HAP can be delivered.
"""
import argparse
import importlib.util
import json
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--inputs',type=Path,required=True)
p.add_argument('--runtime',type=Path,required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('component_builder',repo/'scripts/build_linux_proton_component.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
files={'runtime-modules.list':a.inputs/'runtime-modules.list'}
verified=json.loads((a.inputs/'proton-graphics-build.json').read_text())['files']
aliases={'Xwayland-glx':'Xwayland','libGL-proton.so.1':'libGL.so.1'}
for name in verified:files['graphics/'+aliases.get(name,name)]=a.inputs/name
for name in ['libproton_egl.so','libwayland-egl.so.1','libproton_vulkan.so','libvulkan_virtio.so']:
    files['graphics/'+name]=a.inputs/name
manifest=json.loads((repo/'entry/src/main/resources/rawfile/linux-proton/manifest.json').read_text())
shipped={row['path']:row for row in manifest['files']}
for name,source in files.items():
    assert name in shipped,'Missing delivered artifact: '+name
    assert shipped[name]['sha256']==builder.digest(source),'Stale delivered artifact: '+name
count=builder.validate_native_closure(files,a.runtime)
missing=['libxcb-randr.so.0','libxcb-xfixes.so.0','libxcb-shm.so.0','libxcb-dri3.so.0',
         'libxcb-present.so.0','libxcb-sync.so.1','libxcb-glx.so.0','libXxf86vm.so.1']
for name in missing:
    broken=dict(files);del broken['graphics/'+name]
    try:builder.validate_native_closure(broken,a.runtime)
    except ValueError as error:assert name in str(error),(name,str(error))
    else:raise AssertionError('Missing dependency accepted: '+name)
print('GNU delivered graphics identity, complete ELF closure and all 8 missing-library regressions: PASS',count)
