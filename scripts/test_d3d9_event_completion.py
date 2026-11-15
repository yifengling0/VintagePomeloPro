#!/usr/bin/env python3
"""Run production D3D9 query methods with explicitly scheduled CS/GPU progress."""
import argparse,subprocess,tempfile
from pathlib import Path

def function(text, marker):
    begin=text.index(marker);brace=text.index('{',begin);depth=1;end=brace+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]

def run(source):
    query=(source/'src/d3d9/d3d9_query.cpp').read_text()
    header=(source/'src/d3d9/d3d9_query.h').read_text()
    methods='\n'.join(function(query,m) for m in (
        '  HRESULT STDMETHODCALLTYPE D3D9Query::Issue(',
        '  HRESULT STDMETHODCALLTYPE D3D9Query::GetData(',
        '  HRESULT D3D9Query::GetQueryData(',
        '  void D3D9Query::End(',
        '  bool D3D9Query::QueryBeginnable(',
        '  bool D3D9Query::QueryEndable('))
    signal=(source/'src/util/sync/sync_signal.h').read_text()
    signal=signal[signal.index('  class Signal :'):signal.index('  /**\n   * \\brief Callback signal')]
    members=header[header.index('    D3DQUERYTYPE      m_queryType;'):header.index('    UINT64 GetTimestampQueryFrequency()')]
    cpp=r"""
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>
#include "RC_HEADER"
#include "RC_PTR_HEADER"
namespace dxvk {using mutex=std::mutex;using condition_variable=std::condition_variable;}
namespace dxvk::sync { SIGNAL }
#define STDMETHODCALLTYPE
#define likely(x) (x)
#define unlikely(x) (x)
#define MAKEFOURCC(a,b,c,d) (uint32_t(a)|(uint32_t(b)<<8)|(uint32_t(c)<<16)|(uint32_t(d)<<24))
using DWORD=uint32_t;using UINT64=uint64_t;using BOOL=int32_t;using HRESULT=int32_t;
constexpr HRESULT S_FALSE=1,D3D_OK=0,D3DERR_INVALIDCALL=-1,D3DERR_DEVICELOST=-2;
constexpr int VK_SUCCESS=0;
constexpr BOOL FALSE=0;
constexpr DWORD D3DISSUE_BEGIN=2,D3DISSUE_END=1,D3DGETDATA_FLUSH=1;
enum D3DQUERYTYPE {D3DQUERYTYPE_VCACHE,D3DQUERYTYPE_EVENT,D3DQUERYTYPE_OCCLUSION,D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMPDISJOINT,D3DQUERYTYPE_TIMESTAMPFREQ,D3DQUERYTYPE_VERTEXSTATS};
struct D3DDEVINFO_VCACHE{uint32_t Pattern,OptMethod,CacheSize,MagicNumber;};
struct D3DDEVINFO_D3DVERTEXSTATS{uint32_t NumRenderedTriangles,NumExtraClippingTriangles;};
namespace dxvk {
enum D3D9_VK_QUERY_STATE {D3D9_VK_QUERY_INITIAL,D3D9_VK_QUERY_BEGUN,D3D9_VK_QUERY_ENDED,D3D9_VK_QUERY_CACHED};
union D3D9_QUERY_DATA{D3DDEVINFO_VCACHE VCache;DWORD Occlusion;UINT64 Timestamp;BOOL TimestampDisjoint;UINT64 TimestampFreq;D3DDEVINFO_D3DVERTEXSTATS VertexStats;};
enum class DxvkGpuEventStatus{Invalid,Pending,Signaled};
enum class DxvkGpuQueryStatus{Invalid,Failed,Pending,Available};
struct DxvkQueryData{struct{uint64_t samplesPassed;}occlusion;struct{uint64_t time;}timestamp;struct{uint64_t iaPrimitives,clipPrimitives;}statistic;};
struct Event:RcObject{DxvkGpuEventStatus status=DxvkGpuEventStatus::Pending;DxvkGpuEventStatus test(){return status;}};
using DxvkGpuEvent=Event;
struct DxvkGpuQuery:RcObject{DxvkGpuQueryStatus getData(DxvkQueryData&){return DxvkGpuQueryStatus::Available;}};
struct DxvkContext{
 std::deque<std::pair<Rc<sync::Signal>,uint64_t>> signals;unsigned oldSignals=0;
 void signal(const Rc<sync::Signal>& s,uint64_t value){signals.emplace_back(s,value);}
 void signalGpuEvent(const Rc<Event>&){oldSignals++;}
 void beginQuery(const Rc<DxvkGpuQuery>&){}void endQuery(const Rc<DxvkGpuQuery>&){}void writeTimestamp(const Rc<DxvkGpuQuery>&){}
 void complete(){assert(!signals.empty());auto s=signals.front();signals.pop_front();s.first->signal(s.second);}
};
class D3D9Query;
struct Parent{std::deque<D3D9Query*> queued;unsigned flushes=0;int status=VK_SUCCESS;Parent* GetDXVKDevice(){return this;}int getDeviceStatus(){return status;}void Begin(D3D9Query*){}void End(D3D9Query* p){queued.push_back(p);}void FlushImplicit(BOOL){flushes++;}void pump(DxvkContext&);};
class D3D9Query{
 public:
 static constexpr unsigned MaxGpuQueries=2;
 static constexpr unsigned MaxGpuEvents=1;
 Parent* m_parent;
 MEMBERS
 D3D9Query(Parent* p,bool fallback,D3DQUERYTYPE type=D3DQUERYTYPE_EVENT):m_parent(p),m_queryType(type),m_state(D3D9_VK_QUERY_INITIAL){if(fallback)m_eventCompletion=new sync::Fence(0);else m_event[0]=new Event;}
 HRESULT Issue(DWORD);HRESULT GetData(void*,DWORD,DWORD);HRESULT GetQueryData(void*,DWORD);void End(DxvkContext*);
 static bool QueryBeginnable(D3DQUERYTYPE);static bool QueryEndable(D3DQUERYTYPE);
 void NotifyStall(){}uint64_t GetTimestampQueryFrequency()const{return 1234;}
};
METHODS
void Parent::pump(DxvkContext& ctx){assert(!queued.empty());auto q=queued.front();queued.pop_front();q->End(&ctx);}
}
using namespace dxvk;
int main(){
 Parent parent;DxvkContext ctx;D3D9Query q(&parent,true);
 // API issue and CS recording are not GPU completion.
 assert(q.GetData(nullptr,0,0)==S_FALSE);assert(parent.queued.size()==1);
 parent.pump(ctx);assert(q.m_resetCtr==0);assert(q.GetData(nullptr,0,0)==S_FALSE);
 ctx.complete();BOOL ready=0;assert(q.GetData(&ready,sizeof(ready),0)==D3D_OK&&ready==1);
 ready=0;assert(q.GetData(&ready,sizeof(ready),0)==D3D_OK&&ready==1);
 // An old completion cannot satisfy either queued or recorded reissues.
 q.Issue(D3DISSUE_END);q.Issue(D3DISSUE_END);
 parent.pump(ctx);ctx.complete();assert(q.GetData(nullptr,0,D3DGETDATA_FLUSH)==S_FALSE);
 parent.pump(ctx);assert(q.GetData(nullptr,0,0)==S_FALSE);ctx.complete();assert(q.GetData(nullptr,0,0)==D3D_OK);
 // Repeated ready -> pending -> ready transitions keep distinct targets.
 for(unsigned i=0;i<128;i++){q.Issue(D3DISSUE_END);assert(q.GetData(nullptr,0,0)==S_FALSE);parent.pump(ctx);assert(q.GetData(nullptr,0,0)==S_FALSE);ctx.complete();assert(q.GetData(nullptr,0,0)==D3D_OK);}
 // Device-loss cleanup can notify CPU signals; it must not report success.
 parent.status=-4;assert(q.GetData(nullptr,0,0)==D3DERR_DEVICELOST);
 q.Issue(D3DISSUE_END);parent.pump(ctx);ctx.complete();assert(q.GetData(nullptr,0,0)==D3DERR_DEVICELOST);parent.status=VK_SUCCESS;
 // The default continues using VkEvent and never queues a completion fence.
 Parent oldParent;DxvkContext oldCtx;D3D9Query old(&oldParent,false);
 old.Issue(D3DISSUE_END);oldParent.pump(oldCtx);assert(oldCtx.oldSignals==1&&oldCtx.signals.empty());assert(old.GetData(nullptr,0,0)==S_FALSE);
 old.m_event[0]->status=DxvkGpuEventStatus::Signaled;assert(old.GetData(nullptr,0,0)==D3D_OK);
 // CPU-only query types and begun queries retain the existing state machine.
 Parent otherParent;D3D9Query freq(&otherParent,false,D3DQUERYTYPE_TIMESTAMPFREQ);uint64_t hz=0;assert(freq.GetData(&hz,sizeof(hz),0)==D3D_OK&&hz==1234&&otherParent.queued.empty());
 D3D9Query begun(&otherParent,false,D3DQUERYTYPE_OCCLUSION);begun.Issue(D3DISSUE_BEGIN);assert(begun.GetData(nullptr,0,D3DGETDATA_FLUSH)==S_FALSE&&otherParent.flushes==0);
}
""".replace('RC_HEADER',str(source/'src/util/rc/util_rc.h')).replace('RC_PTR_HEADER',str(source/'src/util/rc/util_rc_ptr.h')).replace('SIGNAL',signal).replace('MEMBERS',members).replace('METHODS',methods)
    with tempfile.TemporaryDirectory(prefix='dxvk-event-contract-') as folder:
        p=Path(folder);(p/'test.cpp').write_text(cpp)
        command=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-pthread','-o',str(p/'test')]
        subprocess.run(command,check=True);subprocess.run([str(p/'test')],check=True)
        # This mutation must be rejected by the reissue assertion above.
        mutant=cpp.replace('>= m_eventIssuedGeneration.load(std::memory_order_relaxed)','>= 1')
        assert mutant!=cpp
        (p/'test.cpp').write_text(mutant);subprocess.run(command,check=True)
        failed=subprocess.run([str(p/'test')],capture_output=True)
        assert failed.returncode!=0 and b'Assertion' in failed.stderr,failed.stderr
    print('PASS: production Issue/GetData/GetQueryData/End, deferred CS/GPU completion, reissue generations, 128 cycles, device loss, cached/default/CPU query paths; ASAN/UBSAN; stale-generation mutation rejected')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk');run(parser.parse_args().source)
