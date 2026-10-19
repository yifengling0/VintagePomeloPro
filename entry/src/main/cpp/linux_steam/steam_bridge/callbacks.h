// Callback fields, not native struct bytes, cross the GNU / Windows ABI.
#pragma once
#include "wire.h"
namespace hmosbridge {
static size_t CallbackSize(int code){switch(code){
case SteamServersConnected_t::k_iCallback:return sizeof(SteamServersConnected_t);
case SteamServerConnectFailure_t::k_iCallback:return sizeof(SteamServerConnectFailure_t);
case SteamServersDisconnected_t::k_iCallback:return sizeof(SteamServersDisconnected_t);
case IPCFailure_t::k_iCallback:return sizeof(IPCFailure_t);
case LicensesUpdated_t::k_iCallback:return sizeof(LicensesUpdated_t);
case ValidateAuthTicketResponse_t::k_iCallback:return sizeof(ValidateAuthTicketResponse_t);
case MicroTxnAuthorizationResponse_t::k_iCallback:return sizeof(MicroTxnAuthorizationResponse_t);
case EncryptedAppTicketResponse_t::k_iCallback:return sizeof(EncryptedAppTicketResponse_t);
case GetAuthSessionTicketResponse_t::k_iCallback:return sizeof(GetAuthSessionTicketResponse_t);
case SteamAPICallCompleted_t::k_iCallback:return sizeof(SteamAPICallCompleted_t);
case UserStatsReceived_t::k_iCallback:return sizeof(UserStatsReceived_t);
case UserStatsStored_t::k_iCallback:return sizeof(UserStatsStored_t);
case UserAchievementStored_t::k_iCallback:return sizeof(UserAchievementStored_t);
case UserStatsUnloaded_t::k_iCallback:return sizeof(UserStatsUnloaded_t);
case PersonaStateChange_t::k_iCallback:return sizeof(PersonaStateChange_t);
case GameOverlayActivated_t::k_iCallback:return sizeof(GameOverlayActivated_t);
case DlcInstalled_t::k_iCallback:return sizeof(DlcInstalled_t);
case GameRichPresenceJoinRequested_t::k_iCallback:return sizeof(GameRichPresenceJoinRequested_t);
default:return 0;}}
template<class T>void CallbackField(Buffer& wire,bool encode,T& value){
 if constexpr(std::is_array_v<T>){static_assert(std::is_same_v<std::remove_extent_t<T>,char>);if(encode)wire.Bytes(value,sizeof(T));else wire.Read(value,sizeof(T));}
 else {if(encode)PutScalar(wire,value);else value=GetScalar<T>(wire);}
}
template<class... T>void CallbackFields(Buffer& wire,bool encode,T&... value){(void)wire;(void)encode;(CallbackField(wire,encode,value),...);}
template<class T,class Fn>Status CallbackType(const void* raw,size_t length,Buffer& wire,bool encode,std::vector<unsigned char>& decoded,Fn&& fn){
 T value{};if(encode){if(length!=sizeof(T)||!raw)return Invalid;memcpy(&value,raw,sizeof(T));}
 fn(wire,encode,value);
 if(!encode){
  if constexpr(std::is_same_v<T,SteamAPICallCompleted_t>){size_t native=CallbackSize(value.m_iCallback);if(!native)return Unsupported;value.m_cubParam=(uint32)native;}
  decoded.resize(sizeof(T));memcpy(decoded.data(),&value,sizeof(T));
 }
 return Ok;
}
static Status CallbackCodec(int code,const void* raw,size_t length,Buffer& wire,bool encode,std::vector<unsigned char>& decoded){switch(code){
case SteamServersConnected_t::k_iCallback:return CallbackType<SteamServersConnected_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,SteamServersConnected_t& v){CallbackFields(w,enc);});
case SteamServerConnectFailure_t::k_iCallback:return CallbackType<SteamServerConnectFailure_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,SteamServerConnectFailure_t& v){CallbackFields(w,enc, v.m_eResult, v.m_bStillRetrying);});
case SteamServersDisconnected_t::k_iCallback:return CallbackType<SteamServersDisconnected_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,SteamServersDisconnected_t& v){CallbackFields(w,enc, v.m_eResult);});
case IPCFailure_t::k_iCallback:return CallbackType<IPCFailure_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,IPCFailure_t& v){CallbackFields(w,enc, v.m_eFailureType);});
case LicensesUpdated_t::k_iCallback:return CallbackType<LicensesUpdated_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,LicensesUpdated_t& v){CallbackFields(w,enc);});
case ValidateAuthTicketResponse_t::k_iCallback:return CallbackType<ValidateAuthTicketResponse_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,ValidateAuthTicketResponse_t& v){CallbackFields(w,enc, v.m_SteamID, v.m_eAuthSessionResponse, v.m_OwnerSteamID);});
case MicroTxnAuthorizationResponse_t::k_iCallback:return CallbackType<MicroTxnAuthorizationResponse_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,MicroTxnAuthorizationResponse_t& v){CallbackFields(w,enc, v.m_unAppID, v.m_ulOrderID, v.m_bAuthorized);});
case EncryptedAppTicketResponse_t::k_iCallback:return CallbackType<EncryptedAppTicketResponse_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,EncryptedAppTicketResponse_t& v){CallbackFields(w,enc, v.m_eResult);});
case GetAuthSessionTicketResponse_t::k_iCallback:return CallbackType<GetAuthSessionTicketResponse_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,GetAuthSessionTicketResponse_t& v){CallbackFields(w,enc, v.m_hAuthTicket, v.m_eResult);});
case SteamAPICallCompleted_t::k_iCallback:return CallbackType<SteamAPICallCompleted_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,SteamAPICallCompleted_t& v){CallbackFields(w,enc, v.m_hAsyncCall, v.m_iCallback, v.m_cubParam);});
case UserStatsReceived_t::k_iCallback:return CallbackType<UserStatsReceived_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,UserStatsReceived_t& v){CallbackFields(w,enc, v.m_nGameID, v.m_eResult, v.m_steamIDUser);});
case UserStatsStored_t::k_iCallback:return CallbackType<UserStatsStored_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,UserStatsStored_t& v){CallbackFields(w,enc, v.m_nGameID, v.m_eResult);});
case UserAchievementStored_t::k_iCallback:return CallbackType<UserAchievementStored_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,UserAchievementStored_t& v){CallbackFields(w,enc, v.m_nGameID, v.m_bGroupAchievement, v.m_rgchAchievementName, v.m_nCurProgress, v.m_nMaxProgress);});
case UserStatsUnloaded_t::k_iCallback:return CallbackType<UserStatsUnloaded_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,UserStatsUnloaded_t& v){CallbackFields(w,enc, v.m_steamIDUser);});
case PersonaStateChange_t::k_iCallback:return CallbackType<PersonaStateChange_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,PersonaStateChange_t& v){CallbackFields(w,enc, v.m_ulSteamID, v.m_nChangeFlags);});
case GameOverlayActivated_t::k_iCallback:return CallbackType<GameOverlayActivated_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,GameOverlayActivated_t& v){CallbackFields(w,enc, v.m_bActive);});
case DlcInstalled_t::k_iCallback:return CallbackType<DlcInstalled_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,DlcInstalled_t& v){CallbackFields(w,enc, v.m_nAppID);});
case GameRichPresenceJoinRequested_t::k_iCallback:return CallbackType<GameRichPresenceJoinRequested_t>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,GameRichPresenceJoinRequested_t& v){CallbackFields(w,enc, v.m_steamIDFriend, v.m_rgchConnect);});
default:return Unsupported;}}
}
