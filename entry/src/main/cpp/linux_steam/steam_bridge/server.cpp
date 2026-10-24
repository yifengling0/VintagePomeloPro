#include "../wine_paths.h"
// Experimental GNU ARM64 helper. Official objects stay inside this process.
#define _GNU_SOURCE 1
#include "sdk.h"
#include "gnu_interfaces.h"
#include "wire.h"
#include "callbacks.h"
#include "warning_queue.h"
extern "C" {
#include "../gnu_fixture/steamclient_probe.h"
}
#include <algorithm>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <map>
#include <functional>
#include <poll.h>
#include <netinet/tcp.h>
#include <set>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <signal.h>
#include <chrono>
#include <unistd.h>
#include <tuple>
#include <cstdio>
#include <cstdlib>
namespace hmosbridge {
// Bounded technical state, separate from the shared (prefix-capped) client log.
// No identity, nonce, ticket, account or callback payload is written here.
static bool sourceVerified = false;
static std::string sourceReport = "null";
static unsigned completedRequests = 0;
static int callbackCode = 0, callbackBytes = 0, callbackResultCode = 0;
static void Report(const char* stage, uint32_t method=0, Status status=Ok) {
 const char* prefix=getenv("STEAMOHOS_WINE_PREFIX");if(!prefix||prefix[0]!='/')return;
 std::string path=std::string(prefix)+"/drive_c/windows/temp/hmosdeck-steam-bridge-state.json",temporary=path+".pending";
 int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return;
 char text[1280];int length=snprintf(text,sizeof(text),"{\"stage\":\"%s\",\"method\":%u,\"status\":%u,\"completedRequests\":%u,\"callbackCode\":%d,\"callbackBytes\":%d,\"callbackResultCode\":%d,\"clientSourceVerified\":%s,\"source\":%s,\"authenticationVerified\":false}\n",stage,method,(unsigned)status,completedRequests,callbackCode,callbackBytes,callbackResultCode,sourceVerified?"true":"false",sourceReport.c_str());
 bool written=length>0&&length<(int)sizeof(text)&&write(fd,text,(size_t)length)==length;int closed=close(fd);
 if(written&&!closed){if(rename(temporary.c_str(),path.c_str()))unlink(temporary.c_str());}else unlink(temporary.c_str());
}
// Proton also requests all callbacks; registration/dispatch stays in steam_api.
static uint32 AllCallbacks(int) { return 1; }
static WarningQueue warningMessages;
static void NativeWarningMessage(int level,const char* message){warningMessages.Push(level,message);}
struct Failure {Status status;};
struct Object {void* pointer;std::string version;int pipe,user;};
class Session {
 void* module=nullptr;void* (*factory)(const char*,int*)=nullptr;ISteamClient* cleanup=nullptr;
 std::string library;std::map<uint32_t,Object> objects;std::map<int,int> users;std::set<int> pipes;uint32_t next=0;
 // Each historical user interface has its own vtable. Ticket cleanup must
 // call through the same ABI that created it, including on disconnect.
 struct TicketOwner {void* target;int pipe;void (*cancel)(void*,HAuthTicket);};
 std::map<HAuthTicket,TicketOwner> tickets;
 std::function<void()> clearWarningHook;int warningPipe=0;
 void ClearWarningHook(){warningMessages.Reset(false);if(clearWarningHook){clearWarningHook();clearWarningHook={};}warningPipe=0;}
 bool verified=false;
 int8_t (*nextCallback)(int,CallbackMsg_t*,int*)=nullptr;
 int8_t (*freeCallback)(int)=nullptr;
 int8_t (*callResult)(int,uint64,void*,int,int,int8_t*)=nullptr;
 void (*releaseMemory)(int)=nullptr;std::set<int> pending;
public:
 explicit Session(const char* path):library(path){
  module=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!module)throw Failure{Invalid};
  factory=(decltype(factory))dlsym(module,"CreateInterface");if(!factory)throw Failure{Invalid};
  nextCallback=(decltype(nextCallback))dlsym(module,"Steam_BGetCallback");
  freeCallback=(decltype(freeCallback))dlsym(module,"Steam_FreeLastCallback");
  callResult=(decltype(callResult))dlsym(module,"Steam_GetAPICallResult");
  releaseMemory=(decltype(releaseMemory))dlsym(module,"Steam_ReleaseThreadLocalMemory");
  if(!nextCallback||!freeCallback||!callResult||!releaseMemory)throw Failure{Unsupported};
  cleanup=(ISteamClient*)factory("SteamClient020",nullptr);if(!cleanup)throw Failure{Unsupported};
  Report("official_module_ready");
 }
 ~Session(){ClearWarningHook();for(auto& [ticket,owner]:tickets)owner.cancel(owner.target,ticket);for(int pipe:pending)freeCallback(pipe);for(auto [pipe,user]:users)cleanup->ReleaseUser(pipe,user);for(int pipe:pipes)cleanup->BReleaseSteamPipe(pipe);}
 Object& Get(uint32_t id,const char* version,bool requireVerified=true){
  auto it=objects.find(id);if(it==objects.end()||it->second.version!=version)throw Failure{Invalid};
  if(requireVerified&&!verified)throw Failure{SourceUnverified};return it->second;
 }
 uint32_t Add(const char* version,void* ptr,int pipe=0,int user=0){
  if(!ptr||!version)return 0;version=CanonicalVersion(version);
  static const char* allowed[]={
#include "versions.inc"
  };
  if(std::none_of(std::begin(allowed),std::end(allowed),[&](const char* name){return !strcmp(name,version);}))return 0;
  if(pipe&&(!verified||!pipes.count(pipe)||(user&&users[pipe]!=user)))throw Failure{Invalid};
  for(auto& [id,o]:objects)if(o.pointer==ptr&&o.version==version&&o.pipe==pipe&&o.user==user)return id;
  if(objects.size()>=4096||next==UINT32_MAX)throw Failure{Invalid};
  uint32_t id=++next;objects.emplace(id,Object{ptr,version,pipe,user});return id;
 }
 uint32_t Factory(const char* version){
  static const char* roots[]={
#include "root_versions.inc"
  };
  if(!version||std::none_of(std::begin(roots),std::end(roots),[&](const char* name){return !strcmp(name,version);}))return 0;
  return Add(version,factory(version,nullptr));
 }
 template<class T>int CreatePipe(T* target){int pipe=target->CreateSteamPipe();if(pipe)pipes.insert(pipe);return pipe;}
 template<class Fn>int CreateUser(Fn create,int& pipe){
  int created=0;int user=create(&created);
  if(!user){if(created&&!pipes.count(created))cleanup->BReleaseSteamPipe(created);pipe=0;return 0;}
  if(!created||pipes.count(created))throw Failure{Invalid};
  char report[512]="{\"status\":\"missing_designation\"}";bool valid=SteamOhosVerifyClientSource(library.c_str(),(void*)factory,report,sizeof(report));
  sourceReport=report;
  if(!valid){cleanup->ReleaseUser(created,user);cleanup->BReleaseSteamPipe(created);throw Failure{SourceUnverified};}
  pipes.insert(created);users[created]=user;verified=true;sourceVerified=true;pipe=created;
  Report("official_local_user_connected");return user;
 }
 template<class T>bool ReleasePipe(T* target,int pipe){
  if(!pipes.count(pipe)||users.count(pipe))throw Failure{Invalid};
  if(clearWarningHook&&warningPipe==pipe)ClearWarningHook();
  bool result=target->BReleaseSteamPipe(pipe);
  if(result){pipes.erase(pipe);Invalidate(pipe,0);}return result;
 }
 template<class T>int ConnectUser(T* target,int pipe){
  if(!pipes.count(pipe)||users.count(pipe))throw Failure{Invalid};int user=target->ConnectToGlobalUser(pipe);if(!user)return 0;
  users[pipe]=user;
  char report[512]="{\"status\":\"missing_designation\"}";verified=SteamOhosVerifyClientSource(library.c_str(),(void*)factory,report,sizeof(report));
  sourceReport=report;
  if(!verified){target->ReleaseUser(pipe,user);users.erase(pipe);throw Failure{SourceUnverified};}
  sourceVerified=true;Report("official_user_connected");
  return user;
 }
 template<class T>void ReleaseUser(T* target,int pipe,int user){
  auto it=users.find(pipe);if(it==users.end()||it->second!=user)throw Failure{Invalid};
  if(clearWarningHook&&warningPipe==pipe)ClearWarningHook();
  for(auto ticket=tickets.begin();ticket!=tickets.end();){if(ticket->second.pipe==pipe){ticket->second.cancel(ticket->second.target,ticket->first);ticket=tickets.erase(ticket);}else ++ticket;}
  if(pending.erase(pipe))freeCallback(pipe);
  target->ReleaseUser(pipe,user);users.erase(it);Invalidate(pipe,user);
 }
 template<bool HasIdentity,class T>Status AuthTicket(Object& entry,T* target,Buffer& input,Buffer& output){
  int capacity=GetScalar<int>(input);SteamNetworkingIdentity identity{};bool identityPresent=false;
  if constexpr(HasIdentity){identityPresent=GetScalar<bool>(input);if(identityPresent)IdentityField(input,false,identity);}
  input.End();if(capacity<0||capacity>65536)return Invalid;
  std::vector<unsigned char> bytes(std::max(1,capacity));uint32 count=0;HAuthTicket ticket;
  if constexpr(HasIdentity)ticket=target->GetAuthSessionTicket(bytes.data(),capacity,&count,identityPresent?&identity:nullptr);
  else ticket=target->GetAuthSessionTicket(bytes.data(),capacity,&count);
  if(count>(uint32)capacity){if(ticket)target->CancelAuthTicket(ticket);return Invalid;}
  if(ticket)tickets.emplace(ticket,TicketOwner{target,entry.pipe,[](void* owner,HAuthTicket value){static_cast<T*>(owner)->CancelAuthTicket(value);}});
  PutScalar(output,ticket);PutScalar(output,count);output.Put(ticket?count:0);if(ticket)output.Bytes(bytes.data(),count);return Ok;
 }
 template<class T>Status EncryptedTicket(Object&,T* target,Buffer& input,Buffer& output){
  int capacity=GetScalar<int>(input);input.End();if(capacity<0||capacity>65536)return Invalid;
  std::vector<unsigned char> bytes((size_t)capacity);uint32 count=0;
  bool result=target->GetEncryptedAppTicket(bytes.data(),capacity,&count);
  if(result&&count>(uint32)capacity)return Invalid;
  PutScalar(output,result);PutScalar(output,count);output.Put(result?count:0);if(result)output.Bytes(bytes.data(),count);return Ok;
 }
 template<class T>HAuthTicket TrackTicket(Object& entry,T* target,HAuthTicket ticket){
  if(ticket)tickets.emplace(ticket,TicketOwner{target,entry.pipe,[](void* owner,HAuthTicket value){static_cast<T*>(owner)->CancelAuthTicket(value);}});return ticket;
 }
 template<class T>void CancelTicket(T* target,HAuthTicket ticket){auto it=tickets.find(ticket);if(it==tickets.end()||it->second.target!=target)throw Failure{Invalid};it->second.cancel(it->second.target,ticket);tickets.erase(it);}
 void ValidatePair(int pipe,int user){if(!verified||!pipes.count(pipe)||(user&&(!users.count(pipe)||users.at(pipe)!=user)))throw Failure{Invalid};}
 template<class T>void RegisterWarningHook(Object& entry,T* target,bool enabled){
  ClearWarningHook();warningMessages.Reset(enabled);
  if(enabled){warningPipe=entry.pipe;clearWarningHook=[target]{target->SetWarningMessageHook(nullptr);};}
  target->SetWarningMessageHook(enabled?NativeWarningMessage:nullptr);
 }
 Status DrainWarnings(int pipe,Buffer& output){
  ValidatePair(pipe,0);if(warningPipe&&warningPipe!=pipe)return Invalid;
  warningMessages.Drain(output);return Ok;
 }
 Status NextCallback(int pipe,Buffer& output){
  ValidatePair(pipe,0);if(pending.count(pipe))return Invalid;
  CallbackMsg_t message{};int ignored=0;
  bool received=nextCallback(pipe,&message,&ignored)!=0;output.Put(received);if(!received)return Ok;
  callbackCode=message.m_iCallback;callbackBytes=message.m_cubParam;callbackResultCode=0;
  if(message.m_iCallback==SteamAPICallCompleted_t::k_iCallback&&message.m_pubParam&&message.m_cubParam==(int)sizeof(SteamAPICallCompleted_t)){
   SteamAPICallCompleted_t value{};memcpy(&value,message.m_pubParam,sizeof(value));callbackResultCode=value.m_iCallback;
  }
  pending.insert(pipe);output.Put((uint32_t)message.m_hSteamUser);output.Put((uint32_t)message.m_iCallback);output.Put((uint32_t)ignored);
  // The PE steam_api owns registration. Only it may decide to discard an
  // unknown callback; never forward opaque GNU layouts into Windows memory.
  bool supported=CallbackSize(message.m_iCallback)!=0;output.Put(supported);
  if(!supported)return Ok;
  std::vector<unsigned char> unused;Status status=CallbackCodec(message.m_iCallback,message.m_pubParam,(size_t)message.m_cubParam,output,true,unused);
  if(status!=Ok){freeCallback(pipe);pending.erase(pipe);}return status;
 }
 bool FreeCallback(int pipe){ValidatePair(pipe,0);if(!pending.count(pipe))return false;bool result=freeCallback(pipe)!=0;pending.erase(pipe);return result;}
 void Memory(){releaseMemory(0);}
 Status CallResult(int pipe,uint64 call,int expected,Buffer& output){
  ValidatePair(pipe,0);size_t size=CallbackSize(expected);if(!size)return Unsupported;
  std::vector<unsigned char> data(size),unused;int8_t failed=0;
  bool result=callResult(pipe,call,data.data(),(int)size,expected,&failed)!=0;
  output.Put(result);output.Put(failed!=0);if(!result)return Ok;
  return CallbackCodec(expected,data.data(),size,output,true,unused);
 }
 void Invalidate(int pipe,int user){for(auto it=objects.begin();it!=objects.end();){if(it->second.pipe==pipe&&(!user||it->second.user==user))it=objects.erase(it);else ++it;}}
};
template<class T,class Enable=void>struct ReadArgument;
template<class T>struct ReadArgument<T,std::enable_if_t<Value<T>>>{T value;explicit ReadArgument(Buffer& input):value(GetValue<T>(input)){}T Get(){return value;}template<class... R>void Output(Buffer&,R...){} };
template<class T>struct ReadArgument<T,std::enable_if_t<ConstValueReference<T>>>{
 using V=std::remove_const_t<std::remove_reference_t<T>>;V value;
 explicit ReadArgument(Buffer& input):value(GetValue<V>(input)){}T Get(){return value;}
 template<class... R>void Output(Buffer&,R...){}
};
template<class T>struct ReadArgument<T,std::enable_if_t<StringArrayPointer<T>>>{
 bool present;SteamParamStringArray_t value{};std::vector<std::string> strings;std::vector<const char*> pointers;
 explicit ReadArgument(Buffer& input):present(GetScalar<bool>(input)){
  if(!present)return;uint64_t count=input.Get();if(count>4096)throw WireError();strings.reserve((size_t)count);
  for(uint64_t i=0;i<count;++i){bool supplied;auto text=input.String(&supplied);if(!supplied)throw WireError();strings.push_back(std::move(text));}
  for(auto& text:strings)pointers.push_back(text.c_str());value.m_nNumStrings=(int32)count;
 }
 T Get(){value.m_ppStrings=pointers.data();return present?&value:nullptr;}
 template<class... R>void Output(Buffer&,R...){}
};
template<class T>struct ReadArgument<const T*,std::enable_if_t<Value<T>&&!std::is_same_v<T,char>>>{
 T value{};bool present;explicit ReadArgument(Buffer& input):present(GetScalar<bool>(input)){if(present)value=GetValue<T>(input);}
 const T* Get(){return present?&value:nullptr;}template<class... R>void Output(Buffer&,R...){}
};
template<class T>struct ReadArgument<InOut<T>,void>{
 T value{};bool present;explicit ReadArgument(Buffer& input):present(GetScalar<bool>(input)){if(present)value=GetValue<T>(input);}
 T* Get(){return present?&value:nullptr;}template<class... R>void Output(Buffer& output,R...){if(present)PutValue(output,value);}
};
template<class T>struct ReadArgument<OutputReferenceValue<T>,void>{
 T value{};explicit ReadArgument(Buffer&){}T& Get(){return value;}
 template<class... R>void Output(Buffer& output,R...){PutValue(output,value);}
};
template<>struct ReadArgument<const char*,void>{std::string value;bool present;explicit ReadArgument(Buffer& input):value(input.String(&present)){}const char* Get(){return present?value.c_str():nullptr;}template<class... R>void Output(Buffer&,R...){} };
template<class T>struct ReadArgument<T*,std::enable_if_t<Value<T>&&!std::is_const_v<T>>>{T value{};bool present;explicit ReadArgument(Buffer& input){uint64_t n=input.Get();if(n>1)throw WireError();present=n;}T* Get(){return present?&value:nullptr;}template<class... R>void Output(Buffer& output,R...){if(present)PutValue(output,value);} };
template<class T,bool O,bool S,bool L>struct ReadArgument<Span<T,O,S,L>,void>{
 using Spec=Span<T,O,S,L>;using E=typename Spec::Element;std::vector<E> value;uint64_t count;bool present;
 explicit ReadArgument(Buffer& input){present=GetScalar<bool>(input);count=input.Get();Spec::CheckCount(count);if(!O&&!present&&count)throw WireError();
  value.resize(std::max<uint64_t>(1,count));if constexpr(!O){if(present){
   if constexpr(sizeof(E)==1&&std::is_integral_v<E>)input.Read(value.data(),(size_t)count);
   else for(uint64_t i=0;i<count;++i)value[i]=GetValue<E>(input);
  }}
 }
 T* Get(){return present?reinterpret_cast<T*>(value.data()):nullptr;}
 template<class... R>void Output(Buffer& output,R... result){if constexpr(O){if(present){
  uint64_t used=count;
  auto limit=[&](auto r){using V=decltype(r);if constexpr(std::is_same_v<V,bool>){if(!r)used=0;}
   else if constexpr(L){if(r<0)used=0;else{if((uint64_t)r>count)throw WireError();used=(uint64_t)r;}}
  };(limit(result),...);if constexpr(S){uint64_t maximum=used;used=0;while(used<maximum&&value[used])++used;if(used<maximum)++used;}
  output.Put(used);
  if constexpr(sizeof(E)==1&&std::is_integral_v<E>)output.Bytes(value.data(),(size_t)used);
  else for(uint64_t i=0;i<used;++i)PutValue(output,value[i]);
 }}}
};
struct AcceptArguments {template<class T>void operator()(const T&)const{}};
template<bool Outputs,class R,class... Args,class Fn,class Check=AcceptArguments>static Status Serve(Fn&& fn,Buffer& input,Buffer& output,Check check={}){
 if constexpr((Argument<Outputs,Args>&&...)&&(Value<R>||std::is_void_v<R>||std::is_same_v<R,const char*>)){
  std::tuple<ReadArgument<Args>...> arguments{ReadArgument<Args>(input)...};input.End();check(arguments);
  auto invoke=[&](auto&... a)->R{return fn(a.Get()...);};
  if constexpr(std::is_void_v<R>){std::apply(invoke,arguments);std::apply([&](auto&... a){(a.Output(output),...);},arguments);}
  else {R result=std::apply(invoke,arguments);
   if constexpr(std::is_same_v<R,const char*>)output.String(result);else PutValue(output,result);
   std::apply([&](auto&... a){(a.Output(output,result),...);},arguments);
  }return Ok;
 }else return Unsupported;
}
// These SDK APIs return filesystem paths. Proton converts them to Windows
// spelling as well. Never return a GNU path to a PE game or invent success for
// a missing/unmapped directory; ownership and user state remain official.
template<class R,bool HasApp,class Fn>static Status ServePathOutput(Fn&& fn,Buffer& input,Buffer& output){
 AppId_t app=0;if constexpr(HasApp)app=GetScalar<AppId_t>(input);
 uint64_t capacity=input.Get();input.End();if(capacity>MaxString)return Invalid;
 std::vector<char> native(MaxString+1,0);R result=fn(app,native.data(),(int)native.size());
 std::string path;
 bool found;
 if constexpr(std::is_same_v<R,int>)found=result>0;
 else found=result!=0;
 if(found){
  size_t length=strnlen(native.data(),native.size());if(length>=native.size())return Invalid;
  char canonical[4096];const char* absolute=realpath(native.data(),canonical)?canonical:native.data();
  const char* prefix=getenv("STEAMOHOS_WINE_PREFIX"),*zroot=getenv("STEAMOHOS_Z_ROOT");
  found=steamohos::WindowsPath(absolute,prefix?prefix:"",zroot?zroot:"",path);
 }
 if(!found){path.clear();if constexpr(std::is_same_v<R,int>)result=-1;else result=0;}
 else if constexpr(std::is_same_v<R,bool>){
  result=path.size()<capacity;if(!result)path.clear();
 }else{
  result=(R)path.size();
  if(!capacity)path.clear();
  else if(path.size()>=capacity)path.resize((size_t)capacity-1);
 }
 PutScalar(output,result);output.String(path.c_str());return Ok;
}
template<class T>static Status ServeAppOwnershipTicket(T* target,Buffer& input,Buffer& output){
 uint64 app=input.Get(),capacity=input.Get();bool bufferPresent=GetScalar<bool>(input),present[4];
 uint32 offsets[4]{};
 for(unsigned i=0;i<4;++i)present[i]=GetScalar<bool>(input);
 input.End();
 if(app>UINT32_MAX||capacity>65536||(!bufferPresent&&capacity))return Invalid;
 std::vector<unsigned char> bytes(std::max<size_t>(1,(size_t)capacity),0);
 uint32 result=target->GetAppOwnershipTicketData((uint32)app,bufferPresent?bytes.data():nullptr,(uint32)capacity,
  present[0]?&offsets[0]:nullptr,present[1]?&offsets[1]:nullptr,present[2]?&offsets[2]:nullptr,present[3]?&offsets[3]:nullptr);
 PutScalar(output,result);for(unsigned i=0;i<4;++i)if(present[i])PutScalar(output,offsets[i]);
 size_t count=bufferPresent&&result<=capacity?result:0;
 output.Put(count);output.Bytes(bytes.data(),count);return Ok;
}
static Status Dispatch(Session& session,uint32_t object,uint32_t method,Buffer& input,Buffer& output){
 if(!method){if(object)return Invalid;bool present;auto version=input.String(&present);input.End();output.Put(present?session.Factory(version.c_str()):0);return Ok;}
 if(method==3){if(object)return Invalid;input.End();session.Memory();return Ok;}
 if(method==WarningMessagesMethod){if(object)return Invalid;int pipe=GetScalar<int32>(input);input.End();return session.DrainWarnings(pipe,output);}
 if(method>=1&&method<=4){if(object)return Invalid;int pipe=(int)GetScalar<int32>(input);
  if(method==1){input.End();return session.NextCallback(pipe,output);}
  if(method==2){input.End();output.Put(session.FreeCallback(pipe));return Ok;}
  if(method==4){uint64 call=GetScalar<uint64>(input);int expected=GetScalar<int32>(input);input.End();return session.CallResult(pipe,call,expected,output);}
 }
 switch(method){
#include "server_methods.inc"
 default:return Unsupported;
 }
}
static bool Transfer(int fd,void* data,size_t count,bool writing){
 char* at=(char*)data;while(count){ssize_t n=writing?send(fd,at,count,MSG_NOSIGNAL):recv(fd,at,count,0);if(n<0&&errno==EINTR)continue;if(n<=0)return false;at+=n;count-=(size_t)n;}return true;
}
}
using namespace hmosbridge;
int main(int argc,char** argv){
 if(argc!=3||strcmp(argv[1],"--endpoint")||argv[2][0]!='/')return 64;
 const char* root=getenv("STEAMOHOS_CODE_ROOT"),*app=getenv("SteamAppId");
 if(!root||root[0]!='/'||!app||!*app||strspn(app,"0123456789")!=strlen(app)||!getenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID"))return 64;
 unsigned long long appValue=strtoull(app,nullptr,10);if(!appValue||appValue>UINT32_MAX)return 64;
 Report("helper_started");
 unsigned char nonce[32];if(getrandom(nonce,sizeof(nonce),0)!=sizeof(nonce))return 65;
 int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return 66;
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);socklen_t size=sizeof(address);
 if(bind(listener,(sockaddr*)&address,size)||listen(listener,1)||getsockname(listener,(sockaddr*)&address,&size))return 66;
 int endpoint=open(argv[2],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(endpoint<0)return 67;
 char text[192],hex[65];for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",nonce[i]);
 int length=snprintf(text,sizeof(text),"HMOSDECK-BRIDGE/1\n%u\n%s\n%s\n",ntohs(address.sin_port),hex,app);
 bool written=write(endpoint,text,(size_t)length)==length;int closed=close(endpoint);if(!written||closed){unlink(argv[2]);return 67;}
 // Private endpoint contents are credentials: never print them or call payloads.
 int result=0;pollfd waiting{listener,POLLIN,0};
 if(poll(&waiting,1,120000)<=0){unlink(argv[2]);close(listener);return 68;}
 Report("waiting_for_game");
 int peer=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);close(listener);unlink(argv[2]);if(peer<0)return 68;
 int noDelay=1;timeval timeout{5,0};
 if(setsockopt(peer,IPPROTO_TCP,TCP_NODELAY,&noDelay,sizeof(noDelay))||setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))||setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))){close(peer);return 68;}
 try{
  std::string library=std::string(root)+"/steamrtarm64/steamclient.so";Session session(library.c_str());uint64_t previous=0;
  // Healthy game sessions are owned by the launch controller, not a fixed
  // play-time or request budget. Socket failure still terminates the helper.
  for(;;){
   pollfd readable{peer,POLLIN,0};int ready=poll(&readable,1,1000);if(ready<0&&errno==EINTR)continue;if(ready<0)break;if(!ready)continue;
   uint32_t length;if(!Transfer(peer,&length,4,false))break;length=ntohl(length);if(length<72||length>MaxFrame)throw WireError();
   Buffer request;request.data.resize(length);if(!Transfer(peer,request.data.data(),length,false))break;
   if(request.Get()!=0x484d534231)throw WireError();uint64_t sequence=request.Get(),object=request.Get(),method=request.Get(),application=request.Get();
   unsigned char provided[32];request.Read(provided,32);unsigned difference=0;for(unsigned i=0;i<32;++i)difference|=nonce[i]^provided[i];
   if(difference||application!=appValue||sequence!=previous+1||object>UINT32_MAX||method>UINT32_MAX)throw WireError();previous=sequence;
   Buffer output;Status status=Invalid;try{status=Dispatch(session,(uint32_t)object,(uint32_t)method,request,output);}catch(const Failure& failure){status=failure.status;}catch(const WireError&){status=Invalid;}
   ++completedRequests;if(completedRequests<=64||status!=Ok)Report("request_completed",(uint32_t)method,status);
   Buffer response;response.Put(0x484d534252);response.Put(sequence);response.Put(status);if(status==Ok)response.Bytes(output.data.data(),output.data.size());
   auto packet=response.Packet();if(!Transfer(peer,packet.data(),packet.size(),true))break;
   if(status!=Ok)fprintf(stderr,"HMOSDECK_STEAM_API method=%u status=%u\n",(uint32_t)method,(unsigned)status);
  }
 }catch(const Failure& failure){Report("official_module_failed",0,failure.status);result=69;}catch(...){Report("helper_failed",0,Invalid);result=69;}
 close(peer);return result;
}
