from pathlib import Path
import os, subprocess, shutil

ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'thirdparty/dxvk-modern/src'
OUT=ROOT/'build/host_tests/dxvk-push-constant-spirv'
OUT.mkdir(parents=True,exist_ok=True)

def function(text, signature):
    a=text.index(signature);b=text.index('{',a);i=b+1;depth=1
    while depth:
        depth+=(text[i]=='{')-(text[i]=='}');i+=1
    return text[a:i]

for name in ['spirv_module.h','spirv_module.cpp','spirv_code_buffer.h','spirv_code_buffer.cpp','spirv_instruction.h']:
    shutil.copyfile(SRC/'spirv'/name,OUT/name)
# Remove Win32 logging/COM includes for this isolated native emitter harness.
(OUT/'spirv_include.h').write_text('''#pragma once
#include <spirv/unified1/spirv.hpp>
#include <spirv/unified1/GLSL.std.450.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
namespace dxvk { constexpr double pi = 3.14159265358979323846; }
''')
compiler=(SRC/'dxbc/dxbc_compiler.cpp').read_text()
code=r'''
#include "spirv_module.h"
#include <fstream>
#include <stdexcept>
namespace dxvk {
enum class DxbcScalarType { Uint32 };
enum class DxbcSystemValue {VertexId,InstanceId};
struct DxbcRegMask {};
struct DxbcRegisterValue {struct{DxbcScalarType ctype;uint32_t ccount;}type;uint32_t id;};
struct DxbcRegisterInfo {struct{DxbcScalarType ctype;uint32_t ccount,alen;}type;spv::StorageClass storageClass;};
struct DxvkError :std::runtime_error {using std::runtime_error::runtime_error;};
namespace str {template<typename...T>std::string format(T&&...){return "error";}}
constexpr uint32_t DxvkDrawIdBiasOffset=4;
struct DxbcCompiler {
 SpirvModule m_module{spvVersion(1,3)};
 struct{uint32_t builtinVertexId=0,builtinBaseVertex=0,builtinInstanceId=0,builtinBaseInstance=0,pushConstantId=0;}m_vs;
 uint32_t getScalarTypeId(DxbcScalarType){return m_module.defIntType(32,0);}
 uint32_t emitNewBuiltinVariable(DxbcRegisterInfo r,spv::BuiltIn builtin,const char* name){
  auto type=m_module.defPointerType(m_module.defIntType(32,0),r.storageClass);
  auto v=m_module.newVar(type,r.storageClass);m_module.decorateBuiltIn(v,builtin);m_module.setDebugName(v,name);return v;
 }
 DxbcRegisterValue emitVsSystemValueLoad(DxbcSystemValue,DxbcRegMask);
 uint32_t emitPushConstants();
};
'''+function(compiler,'  DxbcRegisterValue DxbcCompiler::emitVsSystemValueLoad(')+function(compiler,'  uint32_t DxbcCompiler::emitPushConstants()')+r'''
}
int main(int argc,char**argv){
 if(argc!=2)return 1;
 dxvk::DxbcCompiler c;auto& m=c.m_module;
 m.enableCapability(spv::CapabilityShader);m.enableCapability(spv::CapabilityDrawParameters);
 m.setMemoryModel(spv::AddressingModelLogical,spv::MemoryModelGLSL450);
 auto u=m.defIntType(32,0);auto ptr=m.defPointerType(u,spv::StorageClassOutput);
 auto o0=m.newVar(ptr,spv::StorageClassOutput),o1=m.newVar(ptr,spv::StorageClassOutput);
 m.decorateLocation(o0,0);m.decorateLocation(o1,1);m.decorate(o0,spv::DecorationFlat);m.decorate(o1,spv::DecorationFlat);
 auto v=m.defVoidType();auto f=m.defFunctionType(v,0,nullptr);auto entry=m.allocateId();
 m.functionBegin(v,entry,f,spv::FunctionControlMaskNone);m.opLabel(m.allocateId());
 m.opStore(o0,c.emitVsSystemValueLoad(dxvk::DxbcSystemValue::VertexId,{}).id);
 m.opStore(o1,c.emitVsSystemValueLoad(dxvk::DxbcSystemValue::InstanceId,{}).id);
 m.opReturn();m.functionEnd();m.addEntryPoint(entry,spv::ExecutionModelVertex,"main");
 auto code=m.compile();std::ofstream output(argv[1],std::ios::binary);output.write(reinterpret_cast<const char*>(code.data()),code.size());
}
'''
(OUT/'test.cpp').write_text(code)
subprocess.run(['g++','-std=c++17','-O1','-I',str(SRC.parent/'include/spirv/include'),
  str(OUT/'test.cpp'),str(OUT/'spirv_module.cpp'),str(OUT/'spirv_code_buffer.cpp'),'-o',str(OUT/'emit')],check=True)
subprocess.run([str(OUT/'emit'),str(OUT/'vs-bias.spv')],check=True)
def spirv_tool(name):
    if shutil.which(name):
        return [name]
    container = os.environ.get('WINEHUA_SPIRV_CONTAINER')
    if not container:
        raise RuntimeError('Install SPIR-V tools or set WINEHUA_SPIRV_CONTAINER')
    return ['docker', 'exec', '-i', container, name]

module = (OUT/'vs-bias.spv').read_bytes()
subprocess.run(spirv_tool('spirv-val') + ['--target-env', 'vulkan1.1', '-'], input=module, check=True)
with (OUT/'vs-bias.spvasm').open('wb') as output:
    subprocess.run(spirv_tool('spirv-dis') + ['-'], input=module, check=True, stdout=output)
print('Production VS system-value emitter + SPIR-V builder: Vulkan 1.1 validation PASS')
