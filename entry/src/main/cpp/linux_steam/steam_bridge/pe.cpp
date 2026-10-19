// Experimental lsteamclient backend for the existing Proton loader hook.
// Opt-in per Wine process; DllMain rejects all unconfigured/Windows Steam loads.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include "sdk.h"
#include "wire.h"
#include "callbacks.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
namespace hmosbridge {
static SOCKET connection=INVALID_SOCKET;
static bool attempted=false;
static uint32_t sequence=0,appId=0;
static SteamAPI_CheckCallbackRegistered_t callbackCheck=nullptr;
static unsigned char nonce[32];
static std::recursive_mutex mutex;
static std::map<std::pair<std::string,uint32_t>,void*> interfaces;
static std::map<std::pair<uint32_t,uint32_t>,std::string> strings;
void* Wrap(const char*,uint32_t);
// Bounded technical progress only; never record arguments, account identities or ticket data.
// A PID and monotonic tick distinguish this run from a stale previous game.
static void Report(const char* stage,uint32_t method,Status status){
 if(!appId)return;
 static unsigned reports=0;
 ++reports;if(reports>32 && status==Ok && reports%128)return;
 char text[384];int length=snprintf(text,sizeof(text),
  "{\"stage\":\"%s\",\"method\":%u,\"status\":%u,\"sequence\":%u,\"processId\":%lu,\"monotonicMs\":%llu,\"authenticationVerified\":false}\n",
  stage,method,(unsigned)status,sequence,(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
 if(length<=0||length>=(int)sizeof(text))return;
 const wchar_t* path=L"C:\\windows\\temp\\hmosdeck-steam-api-state.json";
 const wchar_t* pending=L"C:\\windows\\temp\\hmosdeck-steam-api-state.pending";
 HANDLE file=CreateFileW(pending,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return;
 DWORD written=0;BOOL ok=WriteFile(file,text,(DWORD)length,&written,nullptr);CloseHandle(file);
 if(!ok||written!=(DWORD)length||!MoveFileExW(pending,path,MOVEFILE_REPLACE_EXISTING))DeleteFileW(pending);
}
[[noreturn]] static void Failed(uint32_t method,Status status){
 Report("call_failed",method,status);
 char line[160];snprintf(line,sizeof(line),"HMOSDECK_STEAM_API unsupported_or_failed method=%u status=%u\n",method,(unsigned)status);
 OutputDebugStringA(line);SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
 RaiseException(0xe0484d01,EXCEPTION_NONCONTINUABLE,0,nullptr);abort();
}
static bool Transfer(void* data,size_t count,bool writing){
 char* at=(char*)data;
 while(count){int n=writing?send(connection,at,(int)count,0):recv(connection,at,(int)count,0);if(n<=0)return false;at+=n;count-=(size_t)n;}
 return true;
}
static bool Configure(){
 if(attempted)return connection!=INVALID_SOCKET;attempted=true;
 char text[160];DWORD n=GetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",text,sizeof(text));
 if(!n||n>=sizeof(text))return false;
 char* end=nullptr;unsigned long port=strtoul(text,&end,10);
 if(!port||port>65535||!end||*end++!=':'||strlen(end)<66)return false;
 for(unsigned i=0;i<32;++i){
  auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
  int a=digit(end[2*i]),b=digit(end[2*i+1]);if(a<0||b<0)return false;nonce[i]=(unsigned char)(a*16+b);
 }
 if(end[64]!=':')return false;
 const char* app=end+65;if(!*app||strspn(app,"0123456789")!=strlen(app))return false;
 unsigned long long parsed=strtoull(app,nullptr,10);if(!parsed||parsed>UINT32_MAX)return false;appId=(uint32_t)parsed;
 char actual[32];DWORD actualSize=GetEnvironmentVariableA("SteamAppId",actual,sizeof(actual));
 if(!actualSize||actualSize>=sizeof(actual)||strcmp(actual,app))return false;
 WSADATA wsa;if(WSAStartup(MAKEWORD(2,2),&wsa))return false;
 connection=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(connection==INVALID_SOCKET)return false;
 DWORD timeout=5000;
 if(setsockopt(connection,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout))||setsockopt(connection,SOL_SOCKET,SO_SNDTIMEO,(const char*)&timeout,sizeof(timeout))){closesocket(connection);connection=INVALID_SOCKET;return false;}
 sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons((unsigned short)port);target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(connect(connection,(sockaddr*)&target,sizeof(target))){closesocket(connection);connection=INVALID_SOCKET;return false;}
 return true;
}
static Status Exchange(uint32_t object,uint32_t method,Buffer& payload,Buffer& result){
 if(!Configure())return Disconnected;
 Report("request_pending",method,Ok);
 Buffer frame;frame.Put(0x484d534231);frame.Put(++sequence);frame.Put(object);frame.Put(method);frame.Put(appId);frame.Bytes(nonce,32);frame.Bytes(payload.data.data(),payload.data.size());
 uint32_t length=htonl((uint32_t)frame.data.size());
 if(!Transfer(&length,4,true)||!Transfer(frame.data.data(),frame.data.size(),true)||!Transfer(&length,4,false))goto disconnected;
 length=ntohl(length);if(length<24||length>MaxFrame)goto disconnected;
 result.data.resize(length);if(!Transfer(result.data.data(),length,false))goto disconnected;
 if(result.Get()!=0x484d534252||result.Get()!=sequence)goto disconnected;
 {uint64_t status=result.Get();if(status>SourceUnverified)goto disconnected;Report("request_received",method,(Status)status);return (Status)status;}
disconnected:
 closesocket(connection);connection=INVALID_SOCKET;Report("disconnected",method,Disconnected);return Disconnected;
}
template<class T> void WriteArgument(Buffer& out,T value){
 if constexpr(std::is_same_v<T,const char*>)out.String(value);
 else if constexpr(std::is_pointer_v<T>)out.Put(value?1:0);
 else PutScalar(out,value);
}
template<class T> void ReadOutput(Buffer& input,T value){
 if constexpr(std::is_pointer_v<T>&&!std::is_same_v<T,const char*>){if(value)*value=GetScalar<std::remove_pointer_t<T>>(input);}
}
template<bool Outputs,class R,class... Args> static R Invoke(uint32_t object,uint32_t method,Args... args){
 std::lock_guard<std::recursive_mutex> guard(mutex);
 if constexpr((Argument<Outputs,Args>&&...)&&(Scalar<R>||std::is_void_v<R>||std::is_same_v<R,const char*>)){
  Buffer request,response;(WriteArgument(request,args),...);
  Status status=Exchange(object,method,request,response);if(status!=Ok)Failed(method,status);
  if constexpr(std::is_void_v<R>){(ReadOutput(response,args),...);response.End();return;}
  else if constexpr(std::is_same_v<R,const char*>){
   bool present;std::string text=response.String(&present);(ReadOutput(response,args),...);response.End();if(!present)return nullptr;
   auto& cached=strings[{object,method}];cached=std::move(text);return cached.c_str();
  } else {R value=GetScalar<R>(response);(ReadOutput(response,args),...);response.End();return value;}
 }else Failed(method,Unsupported);
}
template<class R>static R Ticket(uint32_t object,uint32_t method,void* buffer,int capacity,uint32* reported){
 std::lock_guard<std::recursive_mutex> guard(mutex);if(!reported||capacity<0||capacity>65536||(!buffer&&capacity))Failed(method,Invalid);
 Buffer request,response;PutScalar(request,capacity);Status status=Exchange(object,method,request,response);if(status!=Ok)Failed(method,status);
 R result=GetScalar<R>(response);uint32 count=GetScalar<uint32>(response);uint64 bytes=response.Get();if(bytes>(uint64)capacity||bytes>65536)Failed(method,Invalid);
 response.Read(buffer,(size_t)bytes);response.End();*reported=count;return result;
}
static void RegisterCallbackCheck(uint32_t object,uint32_t method,SteamAPI_CheckCallbackRegistered_t check){
 Invoke<false,void>(object,method,check!=nullptr);callbackCheck=check;
}
#include "pe_methods.inc"
}
using namespace hmosbridge;
extern "C" __declspec(dllexport) void* __cdecl CreateInterface(const char* version,int* code){
 std::lock_guard<std::recursive_mutex> guard(mutex);if(code)*code=1;
 try {Buffer request,response;request.String(version);if(Exchange(0,0,request,response)!=Ok)return nullptr;
  uint32_t id=(uint32_t)response.Get();response.End();void* value=Wrap(version,id);if(value&&code)*code=0;return value;
 }catch(...){return nullptr;}
}
extern "C" __declspec(dllexport) bool __cdecl Steam_IsKnownInterface(const char* version){
 static const char* names[]={
#include "versions.inc"
 };if(!version)return false;for(auto* name:names)if(!strcmp(version,name))return true;return false;
}
extern "C" __declspec(dllexport) void __cdecl Steam_NotifyMissingInterface(int,const char*){OutputDebugStringA("HMOSDECK_STEAM_API missing interface\n");}
extern "C" __declspec(dllexport) void __cdecl Steam_ReleaseThreadLocalMemory(bool){
 std::lock_guard<std::recursive_mutex> guard(mutex);Buffer request,response;
 if(Exchange(0,3,request,response)!=Ok)SetLastError(ERROR_BROKEN_PIPE);
}
static std::map<int,std::vector<unsigned char>> callbacks;
extern "C" __declspec(dllexport) bool __cdecl Steam_BGetCallback(int pipe,CallbackMsg_t* message,int* ignored){
 std::lock_guard<std::recursive_mutex> guard(mutex);if(!message)return false;
 // Bound one polling call even if the official client emits many private,
 // unregistered notifications. Leave remaining callbacks for the next pump.
 for(unsigned count=0;count<64;++count){
  Buffer request,response;PutScalar(request,pipe);
  Status status=Exchange(0,1,request,response);if(status!=Ok)Failed(1,status);
  bool received=GetScalar<bool>(response);if(!received){response.End();return false;}
  CallbackMsg_t result{};result.m_hSteamUser=GetScalar<int>(response);result.m_iCallback=GetScalar<int>(response);int extra=GetScalar<int>(response);
  bool supported=GetScalar<bool>(response);
  if(!supported){
   response.End();
   if(!callbackCheck||callbackCheck(result.m_iCallback))Failed(1,Unsupported);
   Buffer release,released;PutScalar(release,pipe);status=Exchange(0,2,release,released);
   if(status!=Ok)Failed(2,status);if(!GetScalar<bool>(released))Failed(2,Invalid);released.End();
   continue;
  }
  auto& bytes=callbacks[pipe];status=CallbackCodec(result.m_iCallback,nullptr,0,response,false,bytes);if(status!=Ok)Failed(1,status);response.End();
  result.m_cubParam=(int)bytes.size();result.m_pubParam=bytes.data();*message=result;if(ignored)*ignored=extra;return true;
 }
 return false;
}
extern "C" __declspec(dllexport) bool __cdecl Steam_FreeLastCallback(int pipe){
 std::lock_guard<std::recursive_mutex> guard(mutex);Buffer request,response;PutScalar(request,pipe);
 Status status=Exchange(0,2,request,response);if(status!=Ok)Failed(2,status);bool result=GetScalar<bool>(response);response.End();callbacks.erase(pipe);return result;
}
extern "C" __declspec(dllexport) bool __cdecl Steam_GetAPICallResult(int pipe,uint64 call,void* data,int size,int expected,bool* failed){
 std::lock_guard<std::recursive_mutex> guard(mutex);if(!data||size<0||(size_t)size!=CallbackSize(expected))Failed(4,Unsupported);
 Buffer request,response;PutScalar(request,pipe);PutScalar(request,call);PutScalar(request,expected);
 Status status=Exchange(0,4,request,response);if(status!=Ok)Failed(4,status);
 bool result=GetScalar<bool>(response),failure=GetScalar<bool>(response);if(failed)*failed=failure;
 if(result){std::vector<unsigned char> bytes;status=CallbackCodec(expected,nullptr,0,response,false,bytes);if(status!=Ok||bytes.size()!=(size_t)size)Failed(4,status);memcpy(data,bytes.data(),bytes.size());}response.End();return result;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,void*){
 if(reason==DLL_PROCESS_ATTACH){char marker[160];DWORD n=GetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",marker,sizeof(marker));if(!n||n>=sizeof(marker))return FALSE;DisableThreadLibraryCalls(instance);}
 return TRUE;
}
