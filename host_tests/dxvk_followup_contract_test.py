"""Run extracted production control flow with fake GPU objects; no device use."""
from pathlib import Path
import subprocess
import sys
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'thirdparty/dxvk-modern/src'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/host_tests/dxvk-followup'
OUT.mkdir(parents=True, exist_ok=True)

def function(text, signature):
    a = text.index(signature)
    b = text.index('{', a)
    depth = 1
    i = b + 1
    while depth:
        depth += (text[i] == '{') - (text[i] == '}')
        i += 1
    return text[a:i]

def run(name, code):
    source = OUT / (name + '.cpp')
    source.write_text(code)
    binary = OUT / name
    subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-pthread', '-Wall', '-Wextra',
                    '-I', str(SRC.parent/'include/vulkan/include'),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], timeout=20, check=True)

common = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#define CACHE_LINE_SIZE 64
#define unlikely(x) (x)
namespace dxvk {
using mutex=std::mutex;
using condition_variable=std::condition_variable;
using thread=std::thread;
using high_resolution_clock=std::chrono::steady_clock;
namespace env { void setThreadName(const char*) {} }
namespace str { template<typename... T> std::string format(T&&...) {return "error";} }
struct Logger { static void err(const std::string&) {} };
struct DxvkError : std::runtime_error {
  using std::runtime_error::runtime_error;
  std::string message() const {return what();}
};
enum class DxvkStatCounter { CsSyncCount, CsSyncTicks, CsIdleTicks, CsChunkCount };
template<typename T> struct Rc : std::shared_ptr<T> {
 using std::shared_ptr<T>::shared_ptr;
 T* ptr() const { return this->get(); }
};
struct DxvkDevice {
 std::atomic<bool> failed{false};
 void notifyCommandStreamError() { failed=true; }
 void addStatCtr(DxvkStatCounter, uint64_t) {}
};
struct DxvkContext { void addStatCtr(DxvkStatCounter, uint64_t) {} };
struct Chunk {
 std::function<void()> fn;
 void executeAll(DxvkContext*) { fn(); }
};
using DxvkCsChunkRef=std::shared_ptr<Chunk>;
}
'''

header = (SRC/'dxvk/dxvk_cs.h').read_text()
header = header[header.index('  enum class DxvkCsQueue'):header.rfind('\n}')]
implementation = (SRC/'dxvk/dxvk_cs.cpp').read_text()
implementation = implementation[implementation.index('  DxvkCsThread::DxvkCsThread('):implementation.rfind('\n}')]
cs_test = r'''
using namespace dxvk;
using namespace std::chrono_literals;
template<typename F> auto chunk(F&& fn) {return std::make_shared<Chunk>(Chunk{fn});}
void requireReady(std::future<void>& result) {
 assert(result.wait_for(2s)==std::future_status::ready); result.get();
}
int main() {
 for (int kind=0; kind<3; kind++) for (int repeat=0; repeat<16; repeat++) {
  Rc<DxvkDevice> device(new DxvkDevice);
  Rc<DxvkContext> ctx(new DxvkContext);
  DxvkCsThread cs(device,ctx);
  std::atomic<int> executed{0};
  auto first=cs.dispatchChunk(chunk([&]{++executed;}));
  cs.synchronize(first);
  assert(executed==1 && cs.lastSequenceNumber()==first);
  std::promise<void> entered, release;
  auto permit=release.get_future().share();
  auto failed=cs.dispatchChunk(chunk([&]{
   entered.set_value(); permit.wait();
   if(kind==0) throw DxvkError("injected recording error");
   if(kind==1) throw std::runtime_error("injected standard error");
   throw 1;
  }));
  entered.get_future().wait();
  auto abandoned=cs.dispatchChunk(chunk([&]{executed+=100;}));
  std::vector<std::future<void>> waiters;
  for(int i=0;i<4;i++) waiters.push_back(std::async(std::launch::async,[&]{cs.synchronize(abandoned);}));
  std::atomic<int> injecting{0};
  for(auto q:{DxvkCsQueue::HighPriority,DxvkCsQueue::Ordered})
   waiters.push_back(std::async(std::launch::async,[&,q]{
    injecting++; cs.injectChunk(q,chunk([&]{executed+=1000;}),true);
   }));
  while(injecting!=2) std::this_thread::yield();
  release.set_value();
  for(auto& w:waiters) requireReady(w);
  assert(device->failed && executed==1);
  assert(cs.lastSequenceNumber()==first && cs.lastSequenceNumber()<failed);
  auto after=cs.dispatchChunk(chunk([&]{executed+=10000;}));
  cs.synchronize(after);
  cs.injectChunk(DxvkCsQueue::HighPriority,chunk([&]{executed+=10000;}),true);
  assert(executed==1 && cs.lastSequenceNumber()==first);
 }
 // Successful work also wakes every waiter; a failure is not required.
 Rc<DxvkDevice> d(new DxvkDevice); Rc<DxvkContext> c(new DxvkContext);
 DxvkCsThread cs(d,c);
 std::promise<void> release; auto permit=release.get_future().share();
 auto seq=cs.dispatchChunk(chunk([&]{permit.wait();}));
 std::vector<std::future<void>> waiters;
 for(int i=0;i<8;i++) waiters.push_back(std::async(std::launch::async,[&]{cs.synchronize(seq);}));
 release.set_value(); for(auto& w:waiters) requireReady(w);
 assert(!d->failed && cs.lastSequenceNumber()==seq);
 std::cout<<"CS production thread: 48 injected failures, 6 waiters each, all catches, no false completion, normal broadcast PASS\n";
}
'''
run('cs_failure', common+'namespace dxvk {\n'+header+implementation+'\n}\n'+cs_test)

chunk_source=(SRC/'dxvk/dxvk_cs.cpp').read_text()
chunk_header=(SRC/'dxvk/dxvk_cs.h').read_text()
cmd_class=chunk_header[chunk_header.index('  class DxvkCsCmd {'):chunk_header.index('  template<typename T>')]
run('partial_chunk_cleanup', common+'namespace dxvk {\n'+cmd_class+r'''
enum class DxvkCsChunkFlag { SingleUse };
struct Flags {bool single;bool test(DxvkCsChunkFlag)const{return single;}};
struct DxvkCsChunk {
 Flags m_flags;DxvkCsCmd* m_head;DxvkCsCmd** m_next;uint32_t m_commandOffset=100;
 void executeAll(DxvkContext*);void reset();
};
'''+function(chunk_source,'  void DxvkCsChunk::executeAll(')+function(chunk_source,'  void DxvkCsChunk::reset(')+r'''
}
using namespace dxvk;
struct Cmd: DxvkCsCmd {
 int* executed;int* destroyed;bool fail;
 Cmd(int& e,int& d,bool f):executed(&e),destroyed(&d),fail(f){}
 void exec(DxvkContext*)override{++*executed;if(fail)throw DxvkError("fail after previous command");}
 ~Cmd(){++*destroyed;}
};
int main(){
 for(bool single:{false,true})for(int failAt:{-1,0,1,2}){
  int executed[3]={},destroyed[3]={};
  alignas(Cmd) char bytes[3][sizeof(Cmd)];Cmd* commands[3];
  for(int i=0;i<3;i++)commands[i]=new(bytes[i])Cmd(executed[i],destroyed[i],i==failAt);
  *commands[0]->chain()=commands[1];*commands[1]->chain()=commands[2];
  DxvkCsChunk c{{single},commands[0],nullptr};
  try{c.executeAll(nullptr);assert(failAt==-1);}catch(const DxvkError&){assert(failAt>=0);}
  c.reset();
  for(int i=0;i<3;i++){assert(destroyed[i]==1);assert(executed[i]==int(failAt<0||i<=failAt));}
  assert(!c.m_head && c.m_commandOffset==0);
 }
 std::cout<<"Production chunk cleanup: normal/replay/single-use, failure at each position, exactly one destruction PASS\n";
}
''')

fences=(SRC/'util/sync/sync_signal.h').read_text()
fences=fences[fences.index('namespace dxvk::sync'):]
fence_preamble=common+'namespace dxvk { struct RcObject { virtual ~RcObject() = default; }; }\n'
run('cpu_fence_cancel', fence_preamble+fences+r'''
using namespace std::chrono_literals;
template<typename T> void testFence() {
 T fence;
 std::atomic<bool> failed{false};
 std::vector<std::future<bool>> results;
 for(int i=0;i<6;i++) results.push_back(std::async(std::launch::async,[&]{return fence.wait(10,[&]{return failed.load();});}));
 failed=true;
 for(auto& r:results){assert(r.wait_for(1s)==std::future_status::ready);assert(!r.get());}
 assert(fence.value()==0);
 failed=false;
 auto r=std::async(std::launch::async,[&]{return fence.wait(10,[&]{return failed.load();});});
 fence.signal(10); assert(r.get()); assert(fence.value()==10);
}
int main(){testFence<dxvk::sync::Fence>();testFence<dxvk::sync::CallbackFence>();
 std::cout<<"Production CPU fences: cancellation wakes all without signaling, normal signal PASS\n";}
''')

# Exercise actual conversion selector and metadata for all ordinary color pairs.
context=(SRC/'dxvk/dxvk_context.cpp').read_text()
selector=function(context,'  bool DxvkContext::formatsAreCopyCompatible(')
run('copy_routing',r'''
#include <cassert>
#include <iostream>
#include <vulkan/vulkan_core.h>
struct FormatInfo { VkImageAspectFlags aspectMask; };
const FormatInfo* lookupFormatInfo(VkFormat format) {
 static const FormatInfo c{VK_IMAGE_ASPECT_COLOR_BIT}, d{VK_IMAGE_ASPECT_DEPTH_BIT};
 return format==VK_FORMAT_D32_SFLOAT?&d:&c;
}
struct DxvkContext { static bool formatsAreCopyCompatible(VkFormat,VkFormat); };
'''+selector+r'''
int main(){
 using C=DxvkContext;
 assert(!C::formatsAreCopyCompatible(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8G8B8A8_SNORM));
 assert(C::formatsAreCopyCompatible(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_UNDEFINED));
 assert(C::formatsAreCopyCompatible(VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R32_UINT));
 assert(C::formatsAreCopyCompatible(VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_SNORM));
 assert(!C::formatsAreCopyCompatible(VK_FORMAT_D32_SFLOAT,VK_FORMAT_UNDEFINED));
 assert(!C::formatsAreCopyCompatible(VK_FORMAT_R32_UINT,VK_FORMAT_D32_SFLOAT));
 std::cout<<"Production copy routing: SNORM conversion, raw reinterpret, same format and depth PASS\n";
}
''')

run('packed_copy_range',r'''
#include <vulkan/vulkan_core.h>
#include <cassert>
#include <iostream>
'''+function(context,'  static VkDeviceSize packedCopyViewSize(')+r'''
int main(){
 assert(packedCopyViewSize(16,64,4,{4,4,1},1)==64);
 assert(packedCopyViewSize(16,64,4,{1,1,1},1)==4);
 assert(60+packedCopyViewSize(16,64,4,{1,1,1},1)==64);
 assert(60+packedCopyViewSize(16,64,4,{1,1,1},2)==128);
 assert(packedCopyViewSize(32,128,4,{3,2,2},2)==428);
 assert(packedCopyViewSize(16,64,4,{0,1,1},1)==0);
 assert(packedCopyViewSize(16,64,4,{1,1,1},0)==0);
 std::cout<<"Production packed-copy ranges: full/partial final row, padded pitch, arrays/3D and zero extent PASS\n";
}
''')

(OUT/'source-hashes.json').write_text(json.dumps({str(p.relative_to(SRC)):hashlib.sha256(p.read_bytes()).hexdigest()
    for p in [SRC/'dxvk/dxvk_cs.cpp',SRC/'dxvk/dxvk_cs.h',SRC/'util/sync/sync_signal.h',SRC/'dxvk/dxvk_context.cpp']},indent=2))

api=(SRC/'d3d11/d3d11_context.cpp').read_text()
needs=function(api,'  bool D3D11CommonContext<ContextType>::NeedsInstanceDivisorEmulation()')
divisor=function(api,'  bool D3D11CommonContext<ContextType>::EmitInstanceDivisorDraw(')
run('divisor_rebind', common+r'''
#include <array>
#include <cstring>
using UINT=uint32_t; using INT=int32_t;
constexpr auto VK_VERTEX_INPUT_RATE_INSTANCE=1;
struct DxvkDrawIdBias { uint32_t vertex=0,instance=0; };
constexpr uint32_t DxvkDrawIdBiasOffset=4;
struct VkDrawIndirectCommand {uint32_t vertexCount,instanceCount,firstVertex,firstInstance;};
struct VkDrawIndexedIndirectCommand {uint32_t indexCount,instanceCount,firstIndex;int32_t vertexOffset;uint32_t firstInstance;};
struct Buffer { uint64_t size; };
struct DxvkBufferSlice {
 dxvk::Rc<Buffer> b; uint64_t o=0,l=0;
 DxvkBufferSlice()=default;
 DxvkBufferSlice(dxvk::Rc<Buffer> b_, uint64_t o_, uint64_t l_):b(b_),o(o_),l(l_){}
 auto buffer() const{return b;} auto offset() const{return o;} auto length() const{return l;}
};
struct VertexBuffer : Buffer {
 DxvkBufferSlice GetBufferSlice(uint64_t offset) {
  dxvk::Rc<Buffer> b(new Buffer{size});
  return {b,offset,offset<size?size-offset:0};
 }
};
struct InputBinding {uint32_t binding,inputRate,divisor;};
struct Input {InputBinding value; auto binding() const{return value;}};
struct Layout {
 std::vector<Input> inputs;
 uint32_t GetBindingCount()const{return inputs.size();}
 uint32_t GetAttributeCount()const{return 0;}
 auto GetInput(uint32_t i)const{return inputs.at(i);}
};
struct Features { struct {bool vertexAttributeInstanceRateDivisor=false,vertexAttributeInstanceRateZeroDivisor=false;} extVertexAttributeDivisor; };
struct Device {Features f;const auto& features()const{return f;}};
struct DxvkContext {
 struct Fetch {uint64_t offset,length;uint32_t stride;};
 struct Draw {std::array<Fetch,3> fetch;DxvkDrawIdBias bias;uint32_t base;bool indexed;};
 std::array<Fetch,3> current{}; DxvkDrawIdBias bias{};std::vector<Draw> draws;
 void bindVertexBuffer(uint32_t slot,DxvkBufferSlice slice,uint32_t stride){current.at(slot)={slice.offset(),slice.length(),stride};}
 void pushConstants(uint32_t offset,uint32_t size,const void* data){assert(offset==4 && size==8);std::memcpy(&bias,data,size);}
 void draw(uint32_t count,const VkDrawIndirectCommand* d){assert(count==1 && d->instanceCount==1);draws.push_back({current,bias,d->firstInstance,false});}
 void drawIndexed(uint32_t count,const VkDrawIndexedIndirectCommand* d){assert(count==1 && d->instanceCount==1);draws.push_back({current,bias,d->firstInstance,true});}
};
template<typename ContextType> struct D3D11CommonContext {
 struct Vertex { dxvk::Rc<VertexBuffer> buffer;uint32_t stride,offset;};
 struct {struct{dxvk::Rc<Layout> inputLayout;std::array<Vertex,3> vertexBuffers;}ia;}m_state;
 dxvk::Rc<Device> m_device{new Device};DxvkContext ctx;
 template<typename F> void EmitCs(F&& f){f(&ctx);}
 bool NeedsInstanceDivisorEmulation()const;
 bool EmitInstanceDivisorDraw(bool,UINT,UINT,UINT,INT,UINT);
};
'''+ 'template<typename ContextType>\n'+needs+'\ntemplate<typename ContextType>\n'+divisor+r'''
int main(){
 D3D11CommonContext<int> c;
 c.m_state.ia.inputLayout=dxvk::Rc<Layout>(new Layout{{{{0,1,2}},{{1,1,1}},{{2,1,0}}}});
 // Three-byte records and non-aligned source offsets are legal inputs.
 for(auto& v:c.m_state.ia.vertexBuffers){v={dxvk::Rc<VertexBuffer>(new VertexBuffer{{1ull<<36}}),3,1};}
 for(bool indexed:{false,true}){
  c.ctx.draws.clear();
  assert(c.EmitInstanceDivisorDraw(indexed,6,4,5,-3,4));
  assert(c.ctx.draws.size()==4);
  for(uint32_t i=0;i<4;i++){
   const auto& d=c.ctx.draws[i]; assert(d.indexed==indexed && d.base==4 && d.bias.instance==i && !d.bias.vertex);
   assert(d.fetch[0].offset==1+3*(4+i/2));
   assert(d.fetch[1].offset==1+3*(4+i));
   assert(d.fetch[2].offset==13);
   for(const auto& f:d.fetch)assert(f.stride==0);
  }
  for(const auto& f:c.ctx.current)assert(f.offset==1 && f.stride==3);
  assert(c.ctx.bias.instance==0);
 }
 c.ctx.draws.clear();assert(c.EmitInstanceDivisorDraw(false,3,2,0,0,0x70000000u));
 assert(c.ctx.draws[0].fetch[0].offset==1ull+3ull*0x70000000u);
 c.m_state.ia.vertexBuffers[0].buffer->size=8;
 c.ctx.draws.clear();assert(c.EmitInstanceDivisorDraw(false,3,1,0,0,20));
 assert(c.ctx.draws[0].fetch[0].length==0);
 c.m_device->f.extVertexAttributeDivisor={true,true};
 assert(!c.EmitInstanceDivisorDraw(false,3,4,0,0,4));
 std::cout<<"Production divisor: mixed 0/1/2 steps, byte records, large base, OOB, indexed, ID bias and restore PASS\n";
}
''')

dual_a=context.index('          for (uint32_t i = 0; i < count; i++) {\n            if (runSecondary && runPrimary && preserveOrder)')
dual_b=context.index('\n        } else {\n          throw DxvkError("WineHua: Failed to create dual-source fallback pipelines")',dual_a)
dual=context[dual_a:dual_b]
run('dual_draw_ids',r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
constexpr uint32_t DxvkDrawIdBiasOffset=4,VK_PIPELINE_BIND_POINT_GRAPHICS=0;
struct DxvkDrawIdBias{uint32_t vertex=0,instance=0;};
struct VkDrawIndirectCommand{uint32_t vertexCount,instanceCount,firstVertex,firstInstance;};
struct VkDrawIndexedIndirectCommand{uint32_t indexCount,instanceCount,firstIndex;int32_t vertexOffset;uint32_t firstInstance;};
struct Pipeline{int handle;};
struct Harness{
 DxvkDrawIdBias pushed{};uint32_t drawsIssued=0;
 void pushConstants(uint32_t o,uint32_t s,const void*d){assert(o==4&&s==8);std::memcpy(&pushed,d,s);}
 template<int B> void updatePushConstants(){}
 template<bool Indexed,typename T>void test(const T*draws,uint32_t count,DxvkDrawIdBias originalBias={}){
  const bool runPrimary=true,runSecondary=true,preserveOrder=true,supportsDrawIdBias=true;
  const uint32_t primitiveSize=3;Pipeline secondaryPipeline{1},primaryPipeline{2};
  drawsIssued=0;
  auto emitDraw=[&](const T&d,int pipeline){
   const uint32_t pair=drawsIssued/2,instance=pair/2,primitive=pair%2;
   assert(d.instanceCount==1&&d.firstInstance==11+instance);
   assert(pipeline==int(drawsIssued%2+1));
   assert(pushed.instance==originalBias.instance+instance);
   if constexpr(Indexed){
    assert(d.firstIndex==7+primitive*3&&d.vertexOffset==-3);
    assert(pushed.vertex==originalBias.vertex);
   }else{
    assert(d.firstVertex==7+primitive*3);
    for(uint32_t v=0;v<3;v++)
     assert((d.firstVertex+v)-d.firstVertex+pushed.vertex==primitive*3+v+originalBias.vertex);
   }
   assert((d.firstInstance)-d.firstInstance+pushed.instance==instance+originalBias.instance);
   ++drawsIssued;
  };
'''+dual+r'''
  assert(drawsIssued==8&&pushed.vertex==originalBias.vertex&&pushed.instance==originalBias.instance);
 }
};
int main(){Harness h;VkDrawIndirectCommand d{6,2,7,11};VkDrawIndexedIndirectCommand i{6,2,7,-3,11};
 h.test<false>(&d,1);h.test<true>(&i,1);h.test<false>(&d,1,{0,5});
 std::cout<<"Production dual-source loop: 2 primitives x 2 instances, both passes, indexed/non-indexed, nested bias and restore PASS\n";
}
''')

memory=(SRC/'dxvk/dxvk_memory.cpp').read_text()
a=memory.index('    // Bound the default to the device and guest driver')
b=memory.index('    VkPhysicalDeviceMemoryProperties memInfo',a)
policy=memory[a:b]
version=(SRC/'util/util_version.h').read_text()
version=version[version.index('  class Version {'):version.rfind('\n}')]
run('cached_buffer_policy',common+'namespace str=dxvk::str;\n'+version+r'''
struct Adapter {
 bool venus=true;struct{const char* deviceName="Virtio-GPU Venus (Maleoon 920)";}properties;
 struct{Version driverVersion{25,0,1};}ext;
 auto& deviceProperties(){return properties;}auto& devicePropertiesExt(){return ext;}
 bool isWineHuaVenus(){return venus;}
};
struct Device {Adapter gpu;auto adapter(){return &gpu;}};
std::string setting;
namespace env {std::string getEnvVar(const char*){return setting;}}
struct Logger{static void info(const char*){}};
struct Allocator {
 Device* m_device;bool m_ownCachedBuffers=false;
 bool resolve(){
'''+policy+r'''
  return m_ownCachedBuffers;
 }
};
int main(){
 Device d;Allocator a{&d};assert(a.resolve());
 setting="0";assert(!a.resolve());setting="1";assert(a.resolve());
 setting="";d.gpu.properties.deviceName="Virtio-GPU Venus (Maleoon 910)";assert(!a.resolve());
 setting="1";assert(a.resolve());d.gpu.venus=false;assert(!a.resolve());
 d.gpu.venus=true;setting="";d.gpu.properties.deviceName="Virtio-GPU Venus (Maleoon 920)";
 d.gpu.ext.driverVersion=Version(25,0,2);assert(!a.resolve());
 d.gpu.ext.driverVersion=Version(24,3,0);assert(!a.resolve());
 std::cout<<"Production cached-buffer policy: tested device/driver, 0/1 overrides, other GPUs, versions and non-Venus PASS\n";
}
''')

options=(SRC/'dxbc/dxbc_options.cpp').read_text()
a=options.index('    // This Venus/Maleoon driver')
b=options.index('    overrideQuirk(padCubeDrefCoordinates',a)
ftz_policy=options[a:b]
float_controls=function(options,'    if (options.floatControls)')
override_quirk=function(options,'    const auto overrideQuirk')+';'
run('float_control_policy',common+'namespace str=dxvk::str;\n'+'#include <cstring>\n#include <set>\n'+version+r'''
enum {VK_DRIVER_ID_MESA_VENUS=22,VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE=0};
enum class DxbcFloatControlFlag {PreserveNan32,PreserveNan64,DenormFlushToZero32,DenormPreserve64};
struct Adapter {
 int driver=22;Version version{25,0,1};
 bool matchesDriver(int id,Version min,Version max){return driver==id&&version>=min&&version<max;}
};
std::string setting;
namespace env {std::string getEnvVar(const char*){return setting;}}
struct Logger{static void info(const char*){}};
struct Harness {
 Adapter* adapter;
 struct {struct{struct{const char* deviceName="Virtio-GPU Venus (Maleoon 920)";}properties;}core;
  struct {bool shaderSignedZeroInfNanPreserveFloat32=true,shaderSignedZeroInfNanPreserveFloat64=true;
   int denormBehaviorIndependence=1;bool shaderDenormFlushToZeroFloat32=true,shaderDenormPreserveFloat64=true;}vk12;
 }devInfo;
 struct{bool floatControls=true;}options;
 struct {std::set<DxbcFloatControlFlag> values;void set(DxbcFloatControlFlag f){values.insert(f);}}floatControl;
 void resolve(){floatControl.values.clear();
'''+override_quirk+ftz_policy+float_controls+r'''
 }
};
int main(){
 Adapter a;Harness h{&a};int cases=0;
 for(int driver:{22,19})for(int patch:{0,1,2})for(bool exactName:{false,true})
 for(const char* value:{"","0","1","invalid"})for(bool enabled:{false,true})
 for(bool supported:{false,true})for(int independence:{0,1}){
  a.driver=driver;a.version=Version(25,0,patch);setting=value;
  h.devInfo.core.properties.deviceName=exactName?"Virtio-GPU Venus (Maleoon 920)":"Maleoon 920";
  h.options.floatControls=enabled;h.devInfo.vk12.shaderDenormFlushToZeroFloat32=supported;
  h.devInfo.vk12.denormBehaviorIndependence=independence;h.resolve();
  bool ftz=!(driver==22&&patch==1&&exactName);
  if(setting=="0")ftz=false;
  if(setting=="1")ftz=true;
  assert(bool(h.floatControl.values.count(DxbcFloatControlFlag::DenormFlushToZero32))==(enabled&&supported&&independence&&ftz));
  assert(bool(h.floatControl.values.count(DxbcFloatControlFlag::PreserveNan32))==enabled);
  assert(bool(h.floatControl.values.count(DxbcFloatControlFlag::PreserveNan64))==enabled);
  assert(bool(h.floatControl.values.count(DxbcFloatControlFlag::DenormPreserve64))==(enabled&&independence));
  ++cases;
 }
 std::cout<<"Production FP32 FTZ policy: "<<cases<<" device/driver/override/capability combinations, other float controls preserved PASS\n";
}
''')
