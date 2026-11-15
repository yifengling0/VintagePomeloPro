#!/usr/bin/env python3
"""Check D3D9 compiler metadata against the real render-state layout and mapper.

Compile production setup functions and metadata assignments, including the
descriptor mapper. The old offset-as-size mutation must fail the same contract.
No device or full native DXVK build is required.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

def block(text, signature):
    begin = text.index(signature)
    start = text.index('{', begin)
    level = 1
    end = start + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]

def run(source):
    ff = (source / 'src/d3d9/d3d9_fixed_function.cpp').read_text()
    dxso = (source / 'src/dxso/dxso_compiler.cpp').read_text()
    layout = (source / 'src/dxvk/dxvk_pipelayout.cpp').read_text()
    state = block((source / 'src/d3d9/d3d9_state.h').read_text(), 'struct D3D9RenderStateInfo {') + ';'
    metadata = []
    for compiler in (ff, dxso):
        lines = re.findall(r'    info\.pushConst(?:Offset|Size) = [^;]+;', compiler)
        assert len(lines) == 2
        metadata.append('\n'.join(lines))
    cpp = '''
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>
STATE
enum class DxsoProgramType { VertexShader, PixelShader };
struct ProgramInfo {
 DxsoProgramType kind; unsigned major;
 DxsoProgramType type()const{return kind;} unsigned majorVersion()const{return major;}
};
struct ShaderInfo { uint32_t pushConstOffset=0,pushConstSize=0; };
uint32_t SetupRenderStateBlock(int&, uint32_t count){return count;}
struct D3D9FFShaderCompiler {
 DxsoProgramType m_programType; int m_module=0;
 uint32_t m_pushConstOffset=0,m_pushConstSize=0,m_rsBlock=0;
 void setupRenderStateInfo();
 ShaderInfo info(){ShaderInfo info;FF_META return info;}
};
struct DxsoCompiler {
 ProgramInfo m_programInfo; int m_module=0;
 uint32_t m_pushConstOffset=0,m_pushConstSize=0,m_rsBlock=0;
 void setupRenderStateInfo();
 ShaderInfo info(){ShaderInfo info;DXSO_META return info;}
};
using VkShaderStageFlagBits=uint32_t;
struct Range {uint32_t stageFlags=0,offset=0,size=0;};
struct DxvkDescriptorSlotMapping {
 Range m_pushConstRange;
 void definePushConstRange(VkShaderStageFlagBits stage,uint32_t offset,uint32_t size);
};
FF_SETUP
DXSO_SETUP
MAPPER
int main(){
 D3D9FFShaderCompiler ffv{DxsoProgramType::VertexShader}, ffp{DxsoProgramType::PixelShader};
 DxsoCompiler vs{{DxsoProgramType::VertexShader,2}}, ps2{{DxsoProgramType::PixelShader,2}}, ps3{{DxsoProgramType::PixelShader,3}};
 ffv.setupRenderStateInfo();ffp.setupRenderStateInfo();vs.setupRenderStateInfo();ps2.setupRenderStateInfo();ps3.setupRenderStateInfo();
 auto a=ffv.info(),b=ffp.info(),c=vs.info(),d=ps2.info(),e=ps3.info();
 // These are API data requirements, independent of compiler assignments.
 auto point=offsetof(D3D9RenderStateInfo,pointSize),alpha=offsetof(D3D9RenderStateInfo,alphaRef);
 if(a.pushConstOffset!=point || a.pushConstOffset+a.pushConstSize!=sizeof(D3D9RenderStateInfo))return 1;
 if(b.pushConstOffset!=0 || b.pushConstSize!=point)return 2;
 if(c.pushConstOffset!=point || c.pushConstOffset+c.pushConstSize!=offsetof(D3D9RenderStateInfo,pointScaleA))return 3;
 if(d.pushConstOffset!=0 || d.pushConstSize!=point)return 4;
 if(e.pushConstOffset!=alpha || e.pushConstSize!=sizeof(float))return 5;
 for(auto vertex:{a,c})for(auto pixel:{b,d,e}){
  DxvkDescriptorSlotMapping map;
  // Same size gate as DxvkShader::defineResourceSlots.
  if(vertex.pushConstSize)map.definePushConstRange(1,vertex.pushConstOffset,vertex.pushConstSize);
  if(pixel.pushConstSize)map.definePushConstRange(16,pixel.pushConstOffset,pixel.pushConstSize);
  if(map.m_pushConstRange.stageFlags!=17 || map.m_pushConstRange.offset!=0)return 6;
  if(map.m_pushConstRange.size<std::max(vertex.pushConstOffset+vertex.pushConstSize,pixel.pushConstOffset+pixel.pushConstSize))return 7;
 }
 return 0;
}
'''
    for key, value in {
        'STATE': state, 'FF_META': metadata[0], 'DXSO_META': metadata[1],
        'FF_SETUP': block(ff, 'void D3D9FFShaderCompiler::setupRenderStateInfo()'),
        'DXSO_SETUP': block(dxso, 'void DxsoCompiler::setupRenderStateInfo()'),
        'MAPPER': block(layout, 'void DxvkDescriptorSlotMapping::definePushConstRange('),
    }.items():
        cpp = cpp.replace(key, value)
    with tempfile.TemporaryDirectory(prefix='dxvk-d3d9-push-') as folder:
        p = Path(folder)
        for old in (False, True):
            data = cpp.replace('info.pushConstSize = m_pushConstSize;', 'info.pushConstSize = m_pushConstOffset;') if old else cpp
            (p / 'test.cpp').write_text(data)
            subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
            result = subprocess.run([str(p / 'test')], capture_output=True, text=True)
            assert not result.stderr, result.stderr
            assert result.returncode == (1 if old else 0), (old, result.returncode)
    print('PASS: FF VS/PS, shader VS/PS2/PS3 ranges and all six stage combinations; old mutation rejected; ASAN/UBSAN')

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk')
    run(parser.parse_args().source)
