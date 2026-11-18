#!/usr/bin/env python3
"""Verify and prepare the pinned MIT Win-FG compute subset (no Vulkan layer)."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def prepare_winfg(target):
    lock = json.loads((ROOT/'components/winfg-source.lock.json').read_text())
    source = ROOT/lock['destination']
    for item in lock['files']:
        if digest(source/item['path']) != item['sha256']:
            raise ValueError('Win-FG upstream identity mismatch: '+item['path'])
    for item in lock.get('patches', []):
        if digest(ROOT/item['path']) != item['sha256']:
            raise ValueError('Win-FG patch identity mismatch: '+item['path'])
    for item in lock.get('additionalFiles',[]):
        if digest(ROOT/item['source'])!=item['sha256']:
            raise ValueError('Win-FG shader identity mismatch: '+item['source'])
    target.mkdir(parents=True, exist_ok=True)
    # Only the compute translation unit and headers enter the Gamescope build.
    for item in lock['files']:
        if item['path'].startswith('src/'):
            (target/Path(item['path']).name).write_bytes(
                (source/item['path']).read_bytes().replace(b'\r\n', b'\n'))
        elif item['path'].startswith('shaders/'):
            path=target/item['path']; path.parent.mkdir(exist_ok=True)
            path.write_bytes((source/item['path']).read_bytes().replace(b'\r\n',b'\n'))
    for item in lock.get('patches', []):
        result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '-i',
            str(ROOT/item['path'])], cwd=target, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise ValueError('Win-FG patch failed: '+result.stdout+result.stderr)
    for item in lock.get('adapted', []):
        if digest(target/item['path']) != item['sha256']:
            raise ValueError('Win-FG adapted identity mismatch: '+item['path'])
    for item in lock.get('additionalFiles',[]):
        (target/item['destination']).write_bytes((ROOT/item['source']).read_bytes().replace(b'\r\n',b'\n'))
    # Rebuild the actual compute modules from reviewed GLSL, rather than trust
    # a precompiled header that could diverge from the shipped shader sources.
    names=['of3_luma','of3_downsample','of3_flow','of3_expand','of3_flow_m4',
           'of3_expand_m4','of3_gm_reduce','of3_gm_prewarp','of3_flowreg','wfg_synth','vpp_scene_cut']
    header=['// Generated from pinned and patched Win-FG GLSL.\n#pragma once\n',
            '#include <cstdint>\n#include <cstddef>\nnamespace winfg::embedded {\n']
    with tempfile.TemporaryDirectory(dir=target) as directory:
        for name in names:
            output=Path(directory)/(name+'.spv')
            subprocess.run(['glslangValidator','--target-env','vulkan1.1','-S','comp','-V',
                str(target/'shaders'/(name+'.comp')),'-o',str(output)],check=True,capture_output=True,timeout=30)
            subprocess.run(['spirv-val','--target-env','vulkan1.1',str(output)],check=True,capture_output=True,timeout=30)
            data=output.read_bytes(); words=struct.unpack('<'+'I'*(len(data)//4),data)
            header.append('static const uint32_t '+name.upper()+'_SPV[]={'+','.join(hex(w)+'u' for w in words)+'};\n')
    header.append('struct Blob { const uint32_t* code; size_t words; };\n')
    header.append('enum Shader {'+','.join(n.upper() for n in names)+',SHADER_COUNT};\n')
    header.append('inline Blob blob(Shader s) { switch(s) {\n')
    for name in names:
        symbol=name.upper()+'_SPV'
        header.append('case '+name.upper()+': return {'+symbol+',sizeof('+symbol+')/4};\n')
    header.append('default:return {nullptr,0}; } }\n}\n')
    (target/'embedded_shaders.hpp').write_text(''.join(header))
    return target


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('target', type=Path)
    prepare_winfg(parser.parse_args().target)
