#!/usr/bin/env python3
"""Build the bounded x64 GNU DXVK fixture outside the product HAP."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if not a.cc.name.startswith('x86_64-'):p.error('Use the pinned llvm-mingw x86_64 compiler for ARM64X EC-view qualification')
a.output.parent.mkdir(parents=True,exist_ok=True)
source=ROOT/'smoke/gnu_proton_d3d11_probe.c'
subprocess.run([str(a.cc),'-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'thirdparty/mesa/include/vulkan'),
 '-I'+str(ROOT/'thirdparty/mesa/include'),str(source),'-o',str(a.output),'-static','-Wl,--no-insert-timestamp',
 '-luser32','-lgdi32','-ldxguid'],check=True)
record=dict(sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),bytes=a.output.stat().st_size,
 sha256=hashlib.sha256(a.output.read_bytes()).hexdigest(),architecture='x86_64-windows',role='diagnostic only; not a performance benchmark')
a.output.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record))
