#!/usr/bin/env python3
"""Exercise production attribute compaction and queued FF constant publication."""
from pathlib import Path
import argparse,os,re,subprocess,tempfile

def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def run(source):
    graphics=(source/'src/dxvk/dxvk_graphics.cpp').read_text()
    start=graphics.index('    static const bool pruneVertexInput =')
    end=graphics.index('    VkPipelineVertexInputDivisorStateCreateInfoEXT viDivisorInfo;',start)
    compact=graphics[start:end]
    assert 'viInfo.vertexAttributeDescriptionCount  = viAttribCount;' in graphics
    device=(source/'src/d3d9/d3d9_device.cpp').read_text()
    publish=block(device,'  void D3D9DeviceEx::PublishFixedFunctionConstants(')
    # Calls must follow the final CPU writes in their production branches.
    for write,call in [
        ('data->TweenFactor =','PublishFixedFunctionConstants(m_vsFixedFunction, slice);'),
        ('DecodeD3DCOLOR((D3DCOLOR)rs[D3DRS_TEXTUREFACTOR]','PublishFixedFunctionConstants(m_psFixedFunction, slice);'),
        ('data->Stages[i].BumpEnvLOffset','PublishFixedFunctionConstants(m_psShared, slice);')]:
        first=device.index(write);last=device.index(call,first)
        between=device[first:last]
        assert 'EmitCs(' not in between and last-first<500
    cpp=r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace env {std::string getEnvVar(const char* key){auto p=std::getenv(key);return p?p:"";}}
struct Attribute {uint32_t location,binding,format,offset;
 bool operator==(const Attribute& a)const{return location==a.location&&binding==a.binding&&format==a.format&&offset==a.offset;}
 Attribute description()const{return *this;}
};
struct State {std::vector<Attribute> ilAttributes;struct Layout {uint32_t n;uint32_t attributeCount()const{return n;}}il;};
std::vector<Attribute> compact(const std::vector<Attribute>& attrs,uint32_t mask) {
 State state{attrs,{uint32_t(attrs.size())}};uint32_t m_vsIn=mask;
 std::array<Attribute,32> viAttribs;std::array<uint32_t,32> viBindingMap;
 for(uint32_t i=0;i<32;i++)viBindingMap[i]=(i+3)%32;
COMPACT
 return std::vector<Attribute>(viAttribs.begin(),viAttribs.begin()+viAttribCount);
}
template<class T>using Rc=std::shared_ptr<T>;
struct DxvkBuffer {int id;};
struct DxvkBufferSliceHandle {uintptr_t handle;uint64_t offset,length;void* mapPtr;};
constexpr int VK_SUCCESS=0;
struct DxvkError:std::runtime_error {using std::runtime_error::runtime_error;};
struct DxvkContext {bool fail=false;Rc<DxvkBuffer> buffer;DxvkBufferSliceHandle slice{};
 int flushMappedBuffer(const Rc<DxvkBuffer>& b,const DxvkBufferSliceHandle& s){buffer=b;slice=s;return fail?-1:0;}
};
struct D3D9DeviceEx {
 std::vector<std::function<void(DxvkContext*)>> queue;
 template<class F>void EmitCs(F&& f){queue.emplace_back(std::forward<F>(f));}
 void PublishFixedFunctionConstants(const Rc<DxvkBuffer>&,const DxvkBufferSliceHandle&);
};
PUBLISH
int main() {
 const bool enabled=env::getEnvVar("WINEHUA_DXVK_PRUNE_VERTEX_INPUT")=="1";
 std::vector<Attribute> attrs;
 for(uint32_t i=0;i<18;i++)attrs.push_back({i,i==0||i==4?0u:16u,100+i,4*i});
 auto out=compact(attrs,0xff3u);assert(out.size()==(enabled?10:18));
 uint32_t index=0;
 for(const auto& a:attrs) {
  if(enabled&&!(0xff3u&(1u<<a.location)))continue;
  auto expected=a;expected.binding=(expected.binding+3)%32;
  assert(out[index++]==expected);
 }
 attrs={{7,2,99,12},{0,0,109,0},{4,16,103,16}};
 out=compact(attrs,0x11);assert(out.size()==(enabled?2:3));
 if(enabled){assert(out[0].location==0&&out[1].location==4&&out[1].binding==19);}
 attrs={{31,0,109,8},{32,1,109,12}};
 out=compact(attrs,0x80000000u);assert(out.size()==(enabled?1:2)&&out[0].location==31);
 assert(compact({{0,0,109,0}},0).size()==(enabled?0:1));
 D3D9DeviceEx dev;DxvkContext ctx;char data[64]={};
 auto buffer=std::make_shared<DxvkBuffer>();buffer->id=7;auto original=buffer;
 DxvkBufferSliceHandle slice{17,512,64,data};auto saved=slice;
 data[5]=42;dev.PublishFixedFunctionConstants(buffer,slice);
 assert(!ctx.buffer&&dev.queue.size()==(enabled?1:0));
 buffer=std::make_shared<DxvkBuffer>();slice={99,4096,8,data+32};
 if(enabled) {
  dev.queue[0](&ctx);dev.queue.clear();
  assert(ctx.buffer==original&&ctx.slice.handle==saved.handle&&ctx.slice.offset==512&&ctx.slice.length==64);
  assert(static_cast<char*>(ctx.slice.mapPtr)[5]==42);
  dev.PublishFixedFunctionConstants(buffer,slice);ctx.fail=true;bool thrown=false;
  try{dev.queue[0](&ctx);}catch(const DxvkError&){thrown=true;}assert(thrown);
 }
}
'''.replace('COMPACT',compact).replace('PUBLISH',publish)
    with tempfile.TemporaryDirectory(prefix='dxvk-input-contract-') as folder:
        p=Path(folder);file=p/'test.cpp';file.write_text(cpp)
        command=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']
        subprocess.run(command+[str(file),'-o',str(p/'test')],check=True)
        for flag in ('0','1'):
            subprocess.run([str(p/'test')],env={**os.environ,'WINEHUA_DXVK_PRUNE_VERTEX_INPUT':flag,'WINEHUA_DXVK9_SHADOW_FIXED_FUNCTION':flag},check=True)
        # Removing the pruning condition must break the observed FF contract.
        mutation=re.sub(r'      if \(pruneVertexInput\n.*?        continue;\n','',cpp,count=1,flags=re.S)
        assert mutation!=cpp
        # Keep the now-unused feature flag from being a compile-only mutation failure.
        mutation=mutation.replace('    static const bool pruneVertexInput =','    [[maybe_unused]] static const bool pruneVertexInput =')
        mutation=mutation.replace('uint32_t m_vsIn=mask;', '[[maybe_unused]] uint32_t m_vsIn=mask;')
        file.write_text(mutation)
        subprocess.run(command+[str(file),'-o',str(p/'mutation')],check=True)
        import resource
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result=subprocess.run([str(p/'mutation')],env={**os.environ,'WINEHUA_DXVK_PRUNE_VERTEX_INPUT':'1','WINEHUA_DXVK9_SHADOW_FIXED_FUNCTION':'1'},capture_output=True)
        assert result.returncode==-6 and b'out.size()' in result.stderr,'Old attribute emission must fail the FF attribute count assertion'
    print('PASS: default-off, FF18-to10 attributes, sparse ordering/remap, location31/32, empty input; FF publication ordering, delayed CS/slice capture and error; old-loop mutation rejected; ASAN/UBSAN')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk')
    run(parser.parse_args().source)
