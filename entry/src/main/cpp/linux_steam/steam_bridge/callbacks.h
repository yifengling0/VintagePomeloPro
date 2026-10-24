// Callback fields, not native struct bytes, cross the GNU / Windows ABI.
#pragma once
#include "wire.h"
namespace hmosbridge {
static size_t CallbackSize(int code){switch(code){
#include "callback_sizes.inc"
default:return 0;}}
template<class T>void CallbackField(Buffer& wire,bool encode,T& value){
 if constexpr(std::is_array_v<T>){static_assert(std::is_same_v<std::remove_extent_t<T>,char>);if(encode)wire.Bytes(value,sizeof(T));else wire.Read(value,sizeof(T));}
 else {if(encode)PutScalar(wire,value);else value=GetScalar<T>(wire);}
}
template<class... T>void CallbackFields(Buffer& wire,bool encode,T&... value){(void)wire;(void)encode;(CallbackField(wire,encode,value),...);}
template<class T,class Fn>Status CallbackType(const void* raw,size_t length,Buffer& wire,bool encode,std::vector<unsigned char>& decoded,Fn&& fn){
 T value{};if(encode){if(length!=sizeof(T)||!raw)return Invalid;value=*static_cast<const T*>(raw);}
 fn(wire,encode,value);
 if(!encode){
  if constexpr(std::is_same_v<T,SteamAPICallCompleted_t>){size_t native=CallbackSize(value.m_iCallback);if(!native)return Unsupported;value.m_cubParam=(uint32)native;}
  decoded.resize(sizeof(T));memcpy(decoded.data(),&value,sizeof(T));
 }
 return Ok;
}
static Status CallbackCodec(int code,const void* raw,size_t length,Buffer& wire,bool encode,std::vector<unsigned char>& decoded){switch(code){
#include "callback_codecs.inc"
default:return Unsupported;}}
}
