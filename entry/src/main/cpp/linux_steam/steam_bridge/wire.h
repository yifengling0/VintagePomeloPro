// Experimental, typed Steamworks transport. No native pointer is a wire value.
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace hmosbridge {
constexpr uint32_t MaxFrame=1024*1024, MaxString=16384;
inline const char* CanonicalVersion(const char* version){if(!version)return nullptr;
#include "version_aliases.inc"
 return version;
}
enum Status:uint32_t { Ok=0, Unsupported=1, Invalid=2, Disconnected=3, SourceUnverified=4 };
struct WireError:std::runtime_error { WireError():std::runtime_error("invalid Steam bridge frame"){} };
struct Buffer {
 std::vector<unsigned char> data;size_t at=0;
 void Put(uint64_t n){if(data.size()>MaxFrame-8)throw WireError();for(int i=7;i>=0;--i)data.push_back((unsigned char)(n>>(i*8)));}
 uint64_t Get(){if(at>data.size()||data.size()-at<8)throw WireError();uint64_t n=0;for(unsigned i=0;i<8;++i)n=n<<8|data[at++];return n;}
 void Bytes(const void* p,size_t n){if(n>MaxFrame||data.size()>MaxFrame-n)throw WireError();if(n){if(!p)throw WireError();const auto* b=(const unsigned char*)p;data.insert(data.end(),b,b+n);}}
 void Read(void* p,size_t n){if(at>data.size()||n>data.size()-at||(!p&&n))throw WireError();if(n)memcpy(p,data.data()+at,n);at+=n;}
 void String(const char* s){if(!s){Put(UINT64_MAX);return;}size_t n=strnlen(s,MaxString+1);if(n>MaxString)throw WireError();Put(n);Bytes(s,n);}
 std::string String(bool* present=nullptr){uint64_t n=Get();if(present)*present=n!=UINT64_MAX;if(n==UINT64_MAX)return {};if(n>MaxString||n>data.size()-at)throw WireError();std::string s((char*)data.data()+at,(size_t)n);at+=(size_t)n;if(s.find('\0')!=s.npos)throw WireError();return s;}
 void End()const{if(at!=data.size())throw WireError();}
 // A synchronous RPC must not send its small length prefix separately: the
 // peer needs the body before replying, which otherwise interacts with Nagle.
 std::vector<unsigned char> Packet()const{
  if(data.size()>MaxFrame)throw WireError();
  uint32_t n=(uint32_t)data.size();std::vector<unsigned char> packet;packet.reserve(4+n);
  for(int i=3;i>=0;--i)packet.push_back((unsigned char)(n>>(8*i)));
  packet.insert(packet.end(),data.begin(),data.end());return packet;
 }
};
template<class T> constexpr bool Scalar=std::is_integral_v<T>||std::is_enum_v<T>||std::is_floating_point_v<T>||std::is_same_v<T,CSteamID>||std::is_same_v<T,CGameID>;
template<class T> void PutScalar(Buffer& b,T value){
 if constexpr(std::is_same_v<T,CSteamID>)b.Put(value.ConvertToUint64());
 else if constexpr(std::is_same_v<T,CGameID>)b.Put(value.ToUint64());
 else if constexpr(std::is_floating_point_v<T>){static_assert(sizeof(T)<=8);uint64_t bits=0;memcpy(&bits,&value,sizeof(T));b.Put(bits);}
 else {static_assert(Scalar<T>);b.Put((uint64_t)value);}
}
template<class T> T GetScalar(Buffer& b){
 uint64_t n=b.Get();
 if constexpr(std::is_same_v<T,CSteamID>||std::is_same_v<T,CGameID>)return T((uint64)n);
 else if constexpr(std::is_floating_point_v<T>){T v;memcpy(&v,&n,sizeof(T));return v;}
 else {static_assert(Scalar<T>);if constexpr(std::is_same_v<T,bool>){if(n>1)throw WireError();}return (T)n;}
}
template<class T>struct Record {static constexpr bool supported=false;};
template<class T>constexpr bool Value=Scalar<T>||Record<T>::supported;
template<class T>void ValueField(Buffer& b,bool encode,T& value){
 if constexpr(std::is_array_v<T>){
  using E=std::remove_extent_t<T>;
  if constexpr(std::is_same_v<E,char>||std::is_same_v<E,unsigned char>){if(encode)b.Bytes(value,sizeof(T));else b.Read(value,sizeof(T));}
  else for(auto& element:value)ValueField(b,encode,element);
 }else if constexpr(Scalar<T>){if(encode)PutScalar(b,value);else value=GetScalar<T>(b);}
 else {static_assert(Record<T>::supported,"Unsupported SDK record");Record<T>::Transfer(b,encode,value);}
}
// This public tagged union has a defined active arm. Never transport its
// padding, or use the GNU by-value return ABI at a Windows vtable entry.
template<>struct Record<SteamIPAddress_t>{static constexpr bool supported=true;
 static void Transfer(Buffer& b,bool encode,SteamIPAddress_t& value){
  ValueField(b,encode,value.m_eType);
  if(value.m_eType==k_ESteamIPTypeIPv4)ValueField(b,encode,value.x.m_unIPv4);
  else if(value.m_eType==k_ESteamIPTypeIPv6)ValueField(b,encode,value.x.m_rgubIPv6);
  else throw WireError();
 }
};
template<class T>void PutValue(Buffer& b,T value){ValueField(b,true,value);}
template<class T>std::remove_cv_t<T> GetValue(Buffer& b){std::remove_cv_t<T> value{};ValueField(b,false,value);return value;}
// The extent is an element count, not a native address. Void buffers use
// unsigned bytes. Every call validates it before allocating or invoking Steam.
template<class T,bool Output,bool Text=false,bool ResultCount=false>struct Span {
 using Element=std::conditional_t<std::is_void_v<std::remove_const_t<T>>,unsigned char,std::remove_const_t<T>>;
 T* pointer;uint64_t count;
 static constexpr bool output=Output, text=Text, resultCount=ResultCount;
 static void CheckCount(uint64_t count){if(count>(MaxFrame-4096)/sizeof(Element)||count>(MaxFrame-4096)/(sizeof(Element)==1?1:8))throw WireError();}
 void Validate()const{CheckCount(count);if(!Output&&!pointer&&count)throw WireError();}
};
template<class T,class N>Span<T,false>InputSpan(T* pointer,N count){if(count<0)throw WireError();Span<T,false> result{pointer,(uint64_t)count};result.Validate();return result;}
template<bool Limited=false,class T,class N>Span<T,true,false,Limited>OutputSpan(T* pointer,N count){if(count<0)throw WireError();Span<T,true,false,Limited> result{pointer,(uint64_t)count};result.Validate();return result;}
template<class N>Span<char,true,true>TextSpan(char* pointer,N count){if(count<0)throw WireError();Span<char,true,true> result{pointer,(uint64_t)count};result.Validate();return result;}
// Capacity pointers carry an input value as well as the native result. Keep
// their direction explicit; ordinary output pointers never read game memory.
template<class T>struct InOut {T* pointer;bool read;};
template<class T>InOut<T> InOutValue(T* pointer,bool read=true){return {pointer,read};}
template<class T>struct OutputReferenceValue {T* pointer;};
template<class T>OutputReferenceValue<T> OutputReference(T& value){return {&value};}
template<class T>struct InOutType:std::false_type{};
template<class T>struct InOutType<InOut<T>>:std::bool_constant<Value<T>>{};
template<class T>struct OutputReferenceType:std::false_type{};
template<class T>struct OutputReferenceType<OutputReferenceValue<T>>:std::bool_constant<Value<T>>{};
inline void IdentityField(Buffer& b,bool encode,SteamNetworkingIdentity& v){
 if(encode){PutScalar(b,v.m_eType);PutScalar(b,v.m_cbSize);}else{v={};v.m_eType=GetScalar<ESteamNetworkingIdentityType>(b);v.m_cbSize=GetScalar<int>(b);}
 if(v.m_cbSize<0||v.m_cbSize>(int)sizeof(v.data))throw WireError();
 if(v.m_eType==k_ESteamNetworkingIdentityType_SteamID){if(v.m_cbSize!=8)throw WireError();ValueField(b,encode,v.data.m_steamID64);}
 else if(v.m_eType==k_ESteamNetworkingIdentityType_IPAddress){if(v.m_cbSize!=(int)sizeof(SteamNetworkingIPAddr))throw WireError();ValueField(b,encode,v.data.m_ip.ip.m_ipv6);ValueField(b,encode,v.data.m_ip.m_port);}
 else {if(encode)b.Bytes(&v.data,(size_t)v.m_cbSize);else b.Read(&v.data,(size_t)v.m_cbSize);}
}
template<>struct Record<SteamNetworkingIdentity>{static constexpr bool supported=true;
 static void Transfer(Buffer& b,bool encode,SteamNetworkingIdentity& v){IdentityField(b,encode,v);}
};
template<>struct Record<SteamNetworkingIPAddr>{static constexpr bool supported=true;
 static void Transfer(Buffer& b,bool encode,SteamNetworkingIPAddr& v){ValueField(b,encode,v.ip.m_ipv6);ValueField(b,encode,v.m_port);}
};
template<>struct Record<SteamControllerState_t>{static constexpr bool supported=true;
 static void Transfer(Buffer& b,bool encode,SteamControllerState_t& v){
  ValueField(b,encode,v.unPacketNum);ValueField(b,encode,v.ulButtons);
  ValueField(b,encode,v.sLeftPadX);ValueField(b,encode,v.sLeftPadY);ValueField(b,encode,v.sRightPadX);ValueField(b,encode,v.sRightPadY);
 }
};
#include "record_codecs.inc"
template<class T>struct SpanType:std::false_type{};
template<class T,bool O,bool S,bool L>struct SpanType<Span<T,O,S,L>>:std::bool_constant<Value<typename Span<T,O,S,L>::Element>>{};
template<class T>constexpr bool ConstValueReference=std::is_lvalue_reference_v<T>&&std::is_const_v<std::remove_reference_t<T>>&&Value<std::remove_const_t<std::remove_reference_t<T>>>;
template<class T>constexpr bool ConstValuePointer=std::is_pointer_v<T>&&std::is_const_v<std::remove_pointer_t<T>>&&Value<std::remove_const_t<std::remove_pointer_t<T>>>;
template<class T>constexpr bool StringArrayPointer=std::is_same_v<T,const SteamParamStringArray_t*>||std::is_same_v<T,SteamParamStringArray_t*>;
template<bool Outputs,class T> constexpr bool Argument=Value<T>||ConstValueReference<T>||ConstValuePointer<T>||std::is_same_v<T,const char*>||StringArrayPointer<T>||SpanType<T>::value||InOutType<T>::value||OutputReferenceType<T>::value||(Outputs&&std::is_pointer_v<T>&&Value<std::remove_pointer_t<T>>&&!std::is_const_v<std::remove_pointer_t<T>>);
}
