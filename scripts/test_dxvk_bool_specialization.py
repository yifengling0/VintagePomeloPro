#!/usr/bin/env python3
"""Execute the production bool rewrite against valid SPIR-V with fog on/off.

Requires g++, spirv-as and spirv-val. No device, Wine or full DXVK build needed.
"""
import argparse
import re
import struct
import subprocess
import tempfile
from pathlib import Path

def run(source):
    shader = (source / 'src/dxvk/dxvk_shader.cpp').read_text()
    begin = shader.index('  static bool freezeBoolSpecConstants(')
    end = shader.index('  // Bake the two per-pipeline BGRA', begin)
    function = shader[begin:end]
    header = (source / 'src/dxvk/dxvk_shader.h').read_text()
    enum = header[header.index('  enum class DxvkSpecConstantId'):header.index('  /**\n   * \\brief Shader flags')]
    limits = (source / 'src/dxvk/dxvk_limits.h').read_text()
    limits = limits[limits.index('namespace dxvk {') + len('namespace dxvk {'):limits.rindex('}')]
    resource_count = int(re.search(r'MaxNumResourceSlots\s*=\s*(\d+)', limits)[1])
    target_count = int(re.search(r'MaxNumRenderTargets\s*=\s*(\d+)', limits)[1])
    pipeline_count = int(re.search(r'MaxNumSpecConstants\s*=\s*(\d+)', limits)[1])
    first = resource_count + target_count + 1
    assembly = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color
OpExecutionMode %main OriginUpperLeft
OpName %fog "fog"
OpName %bound "bound"
OpName %other "other"
OpDecorate %color Location 0
OpDecorate %fog SpecId FOG_ID
OpDecorate %bound SpecId 0
OpDecorate %other SpecId OTHER_ID
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%ptr = OpTypePointer Output %v4
%zero = OpConstant %float 0
%one = OpConstant %float 1
%red = OpConstantComposite %v4 %one %zero %zero %one
%green = OpConstantComposite %v4 %zero %one %zero %one
%fog = OpSpecConstantFalse %bool
%bound = OpSpecConstantTrue %bool
%other = OpSpecConstantTrue %bool
%color = OpVariable %ptr Output
%main = OpFunction %void None %fn
%label = OpLabel
%choice = OpSelect %float %fog %one %zero
%v = OpCompositeConstruct %v4 %choice %choice %zero %one
OpStore %color %v
OpReturn
OpFunctionEnd
'''.replace('FOG_ID', str(first + 2)).replace('OTHER_ID', str(first + pipeline_count + 10))
    cpp = r'''
#include <cstdint>
#include <vector>
#include <bitset>
#include <fstream>
#include <cassert>
#include <spirv/spirv.hpp>
namespace dxvk {
LIMITS
ENUM
class SpirvCodeBuffer { std::vector<uint32_t> v; public:
 SpirvCodeBuffer(size_t n,const uint32_t* p):v(p,p+n){};
 const uint32_t* data()const{return v.data();}size_t dwords()const{return v.size();}
};
struct DxvkBindingMask {std::bitset<MaxNumResourceSlots> v;bool test(size_t n)const{return v.test(n);}};
FUNCTION
}
using namespace dxvk;
int main(int argc,char** argv){assert(argc==4);
 std::ifstream f(argv[1],std::ios::binary|std::ios::ate);size_t bytes=f.tellg();assert(bytes%4==0);
 std::vector<uint32_t> input(bytes/4);f.seekg(0);f.read((char*)input.data(),bytes);
 SpirvCodeBuffer buffer(input.size(),input.data());DxvkBindingMask mask;
 uint32_t pipeline[MaxNumSpecConstants]={};pipeline[2]=argv[3][0]=='1';
 assert(freezeBoolSpecConstants(buffer,&mask,1,argv[3][0]=='n'?nullptr:pipeline));
 std::ofstream out(argv[2],std::ios::binary);out.write((char*)buffer.data(),buffer.dwords()*4);
}
'''.replace('LIMITS', limits).replace('ENUM', enum).replace('FUNCTION', function)
    with tempfile.TemporaryDirectory(prefix='dxvk-bool-state-') as folder:
        p = Path(folder)
        (p / 'input.spvasm').write_text(assembly)
        (p / 'test.cpp').write_text(cpp)
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(p / 'input.spvasm'), '-o', str(p / 'input.spv')], check=True)
        subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I' + str(source / 'include'), str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
        for mode in ('0', '1', 'null'):
            out = p / (mode + '.spv')
            subprocess.run([str(p / 'test'), str(p / 'input.spv'), str(out), mode], check=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(out)], check=True)
            words = struct.unpack('<' + 'I' * (out.stat().st_size // 4), out.read_bytes())
            names, values = {}, {}
            at = 5
            while at < len(words):
                n, op = words[at] >> 16, words[at] & 65535
                if op == 5:
                    names[words[at + 1]] = struct.pack('<' + 'I' * (n - 2), *words[at + 2:at + n]).split(b'\0')[0].decode()
                if op in (41, 42):
                    values[words[at + 2]] = op == 41
                assert op not in (48, 49), 'bool specialization not rewritten'
                assert not (op == 71 and n == 4 and words[at + 2] == 1), 'SpecId left on a constant'
                at += n
            actual = {names[k]: v for k, v in values.items() if k in names}
            assert actual == {'fog': mode == '1', 'bound': False, 'other': True}, (mode, actual)
    print('PASS: fog on/off, binding mask, absent pipeline defaults, unrelated bool defaults; ASAN/UBSAN; SPIR-V validation')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'thirdparty/dxvk')
    run(parser.parse_args().source)
