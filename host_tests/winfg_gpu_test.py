#!/usr/bin/env python3
"""Host-only execution of the reviewed Win-FG graph; no device/FPS claim."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from winfg_source import prepare_winfg

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--vulkan-headers',required=True,type=Path)
    args=parser.parse_args()
    out=ROOT/'build/host-tests/winfg'; out.mkdir(parents=True,exist_ok=True)
    prepare_winfg(out/'winfg')
    binary=out/'gpu-test'
    subprocess.run(['g++','-std=c++20','-O2','-Wall','-Wextra','-Wno-missing-field-initializers',
        '-I'+str(args.vulkan_headers),'-I'+str(out),'-I'+str(ROOT/'native/gamescope/ohos'),
        str(ROOT/'host_tests/winfg_gpu_test.cpp'),str(out/'winfg/framegen.cpp'),
        '-l:libvulkan.so.1','-o',str(binary)],check=True,timeout=60,capture_output=True)
    icd=Path('/usr/share/vulkan/icd.d/lvp_icd.json')
    if not icd.is_file(): raise ValueError('Host lavapipe required')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    env=dict(os.environ,VK_DRIVER_FILES=str(icd),VK_ICD_FILENAMES=str(icd))
    result=subprocess.run([str(binary)],env=env,capture_output=True,text=True,timeout=100)
    (out/'gpu.log').write_text(result.stdout+result.stderr)
    if result.returncode: raise RuntimeError((result.stdout+result.stderr)[-5000:])
    report=dict(scope='host Vulkan correctness only; device performance unqualified',
        winfgLockSha256=hashlib.sha256((ROOT/'components/winfg-source.lock.json').read_bytes()).hexdigest(),
        sourceSha256={n:hashlib.sha256((out/'winfg'/n).read_bytes()).hexdigest()
            for n in ['framegen.cpp','record_impl.inc','embedded_shaders.hpp']},
        result=result.stdout.splitlines())
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(result.stdout)

if __name__=='__main__': main()
