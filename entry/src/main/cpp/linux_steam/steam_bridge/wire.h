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
enum Status:uint32_t { Ok=0, Unsupported=1, Invalid=2, Disconnected=3, SourceUnverified=4 };
struct WireError:std::runtime_error { WireError():std::runtime_error("invalid Steam bridge frame"){} };
struct Buffer {
 std::vector<unsigned char> data;size_t at=0;
 void Put(uint64_t n){if(data.size()>MaxFrame-8)throw WireError();for(int i=7;i>=0;--i)data.push_back((unsigned char)(n>>(i*8)));}
 uint64_t Get(){if(at>data.size()||data.size()-at<8)throw WireError();uint64_t n=0;for(unsigned i=0;i<8;++i)n=n<<8|data[at++];return n;}
 void Bytes(const void* p,size_t n){if(n>MaxFrame||data.size()>MaxFrame-n)throw WireError();if(n){if(!p)throw WireError();const auto* b=(const unsigned char*)p;data.insert(data.end(),b,b+n);}}
 void Read(void* p,size_t n){if(n>data.size()-at||(!p&&n))throw WireError();if(n)memcpy(p,data.data()+at,n);at+=n;}
 void String(const char* s){if(!s){Put(UINT64_MAX);return;}size_t n=strnlen(s,MaxString+1);if(n>MaxString)throw WireError();Put(n);Bytes(s,n);}
 std::string String(bool* present=nullptr){uint64_t n=Get();if(present)*present=n!=UINT64_MAX;if(n==UINT64_MAX)return {};if(n>MaxString||n>data.size()-at)throw WireError();std::string s((char*)data.data()+at,(size_t)n);at+=(size_t)n;if(s.find('\0')!=s.npos)throw WireError();return s;}
 void End()const{if(at!=data.size())throw WireError();}
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
template<bool Outputs,class T> constexpr bool Argument=Scalar<T>||std::is_same_v<T,const char*>||(Outputs&&std::is_pointer_v<T>&&Scalar<std::remove_pointer_t<T>>&&!std::is_const_v<std::remove_pointer_t<T>>);
}
