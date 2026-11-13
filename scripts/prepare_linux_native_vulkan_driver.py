#!/usr/bin/env python3
"""Describe the existing GNU Proton Venus closure; never duplicate its archive.

The app verifies and installs only these libraries beside the native FEX loader.
This keeps OHOS executable-memory scope within its selected runtime release.
"""
import argparse, json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
NAMES=['libvulkan_virtio.so','libxcb-dri3.so.0','libxcb-present.so.0',
       'libxcb-xfixes.so.0','libxcb-sync.so.1','libxcb-randr.so.0','libxcb-shm.so.0',
       'Xwayland','libGL.so.1','libXxf86vm.so.1','libdril_dri.so',
       'libgallium-25.0.1.so','libgbm.so.1','libxcb-glx.so.0']
RELATIVE='linux-steam/bootstrap/native-vulkan-driver/manifest.json'
def expected(root):
 raw=root/'entry/src/main/resources/rawfile'
 proton=json.loads((raw/'linux-proton/manifest.json').read_text())
 rows={x['path']:x for x in proton['files']}
 return dict(schemaVersion=1,architecture='aarch64-linux-gnu',source='linux-proton/gnu-proton.zip',
             files={name:{k:rows['graphics/'+name][k] for k in ['bytes','sha256']} for name in NAMES})
def verify(root=ROOT,hap=None):
 raw=root/'entry/src/main/resources/rawfile';path=raw/RELATIVE
 if path.is_symlink() or json.loads(path.read_text())!=expected(root):raise ValueError('Native Vulkan driver manifest mismatch')
 if hap and hap.read('resources/rawfile/'+RELATIVE)!=path.read_bytes():raise ValueError('HAP native Vulkan driver manifest mismatch')
 return expected(root)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verify-only',action='store_true');a=p.parse_args()
 path=ROOT/'entry/src/main/resources/rawfile'/RELATIVE
 if not a.verify_only:
  text=json.dumps(expected(ROOT),indent=2)+'\n'
  if not path.is_file() or path.read_text()!=text:
   path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
 result=verify();print('Verified native Vulkan closure from existing GNU package:',sum(x['bytes'] for x in result['files'].values()),'bytes')
if __name__=='__main__':main()
