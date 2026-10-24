"""Behavioral tests of the experimental bridge using explicitly fake SDK objects.

No mock success is real-device SteamAPI, ownership, callback or ticket acceptance.
Set STEAM_BRIDGE_REFERENCE and STEAM_BRIDGE_MINGW to the pinned local SDK/toolchain.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
ROOT = Path(__file__).resolve().parents[1]
Q = lambda value: struct.pack("!Q", value)
def text(value):
    raw = value.encode()
    return Q(len(raw)) + raw
MOCK = r"""
#include "sdk.h"
#include "mock_interfaces.inc"
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int callback=0;
static void Event(const char* event){FILE* f=fopen(getenv("MOCK_TRACE"),"a");if(f){fprintf(f,"%s\n",event);fclose(f);}}
static void WarningHook(SteamAPIWarningMessageHook_t fn){Event(fn?"warning_registered":"warning_unregistered");if(fn)fn(1,"official fixture warning");}
#include "gnu_stubs.h"
class User:public StubGnuISteamUser021{public:
 bool BLoggedOn()override{return true;}
 CSteamID GetSteamID()override{return CSteamID((uint64)76561198000000042ull);}
 HSteamUser GetHSteamUser()override{return 456;}
 bool GetUserDataFolder(char* buffer,int capacity)override{const char* folder=getenv("MOCK_USER_PATH");if(!folder||capacity<=(int)strlen(folder))return false;strcpy(buffer,folder);return true;}
 HAuthTicket GetAuthSessionTicket(void* output,int capacity,uint32* count)override{if(capacity<4)return 0;memset(output,0xa5,4);*count=4;callback=163;Event("ticket_create");return 987;}
 void CancelAuthTicket(HAuthTicket h)override{if(h!=987)abort();Event("ticket_cancel");}
 void TrackAppUsageEvent(CGameID& game,int event,const char* extra)override{if(game.ToUint64()!=480||event!=17||strcmp(extra,"abi"))abort();Event("gnu_gameid_reference");}
};
class Apps:public StubISteamApps{public:
 bool BIsSubscribedApp(AppId_t app)override{return app==480;}
 uint32 GetAppInstallDir(AppId_t app,char* buffer,uint32 capacity)override{const char* folder=getenv("MOCK_APP_PATH");if(app!=480||!folder)return 0;if(capacity)snprintf(buffer,capacity,"%s",folder);return strlen(folder);}
};
class Stats:public StubISteamUserStats{public:
 bool GetAchievement(const char* name,bool* out)override{if(!out)return false;*out=!strcmp(name,"fixture");return true;}
 bool RequestCurrentStats()override{callback=getenv("MOCK_PRIVATE_CALLBACK")?1040044:1101;return true;}
};
static User user;static Apps apps;static Stats stats;
class Storage:public StubISteamRemoteStorage014{public:
 bool FileWrite(const char* name,const void* bytes,int32 size)override{Event("file_write");return !strcmp(name,"save")&&size==3&&!memcmp(bytes,"a\0b",3);}
 int32 FileRead(const char* name,void* bytes,int32 size)override{Event("file_read");if(strcmp(name,"save")||size<3)return 0;memcpy(bytes,"a\0b",3);return 3;}
};
static Storage storage;
class ModernUser:public StubISteamUser023{public:
 bool BLoggedOn()override{return true;}
 CSteamID GetSteamID()override{return CSteamID((uint64)76561198000000044ull);}
 HAuthTicket GetAuthSessionTicket(void* bytes,int capacity,uint32* count,const SteamNetworkingIdentity* identity)override{
  if(identity&&(identity->m_eType!=k_ESteamNetworkingIdentityType_SteamID||identity->m_cbSize!=8||identity->data.m_steamID64!=42))abort();
  Event(identity?"modern_identity":"modern_no_identity");if(capacity<3)return 0;memcpy(bytes,"x\0z",3);*count=3;return 991;}
 HAuthTicket GetAuthTicketForWebApi(const char* identity)override{Event("web_ticket");return !strcmp(identity,"service")?992:0;}
 void CancelAuthTicket(HAuthTicket ticket)override{if(ticket!=991&&ticket!=992)abort();Event("modern_cancel");}
};
static ModernUser modernUser;

class LegacyUser:public StubISteamUser017{public:
 bool BLoggedOn()override{return true;}
 CSteamID GetSteamID()override{return CSteamID((uint64)76561198000000043ull);}
 HAuthTicket GetAuthSessionTicket(void* output,int capacity,uint32* count)override{
  if(capacity<3)return 0;memset(output,0x5a,3);*count=3;Event("legacy_ticket_create");return 654;}
 void CancelAuthTicket(HAuthTicket ticket)override{if(ticket!=654)abort();Event("legacy_ticket_cancel");}
};
class LegacyUtils:public StubISteamUtils007{public:AppId_t GetAppID()override{return 480;}void SetOverlayNotificationInset(int x,int y)override{if(x!=7||y!=9)abort();Event("legacy_utils_inset");}void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn)override{WarningHook(fn);}};
class LegacyApps:public StubISteamApps006{public:bool BIsSubscribedApp(AppId_t app)override{return app==480;}};
static LegacyUser legacyUser;static LegacyUtils legacyUtils;static LegacyApps legacyApps;
class AppTicket:public StubISteamAppTicket{public:
 uint32 GetAppOwnershipTicketData(uint32 app,void* output,uint32 capacity,uint32* a,uint32* u,uint32* s,uint32* n)override{
  Event("ownership_ticket_called");
  if(a)*a=app==480?4:0;if(u)*u=app==480?8:0;if(s)*s=app==480?16:0;if(n)*n=app==480?8:0;
  if(app!=480)return 0;
  if(output&&capacity>=24)for(unsigned i=0;i<24;++i)static_cast<unsigned char*>(output)[i]=(unsigned char)i;
  return 24;
 }
};
static AppTicket appTicket;
class Inventory:public StubGnuISteamInventory003{public:
 bool GetItemDefinitionIDs(SteamItemDef_t* items,uint32* count)override{
  Event("inventory_ids");if(!count)abort();uint32 capacity=*count;*count=3;
  if(!items)return true;if(capacity<3)return false;items[0]=11;items[1]=22;items[2]=33;return true;}
 bool GenerateItems(SteamInventoryResult_t* result,const SteamItemDef_t* ids,const uint32* counts,uint32 size)override{
  if(!result||size!=2||ids[0]!=11||ids[1]!=22||counts[0]!=1||counts[1]!=2)abort();*result=77;Event("inventory_generate");return true;}
};
class UGC:public StubGnuISteamUGC021{public:
 bool SetItemTags(UGCUpdateHandle_t handle,const SteamParamStringArray_t* tags,bool allowAdmin)override{
  Event("ugc_tags");if(handle!=91||allowAdmin)abort();if(!tags)return false;
  if(tags->m_nNumStrings==0)return true;
  if(tags->m_nNumStrings!=2||strcmp(tags->m_ppStrings[0],"one")||strcmp(tags->m_ppStrings[1],"two"))abort();return true;}
};
class HTTP:public StubGnuISteamHTTP003{public:
 bool GetHTTPResponseBodyData(HTTPRequestHandle h,uint8* bytes,uint32 size)override{
  if(h!=3||size!=4)abort();memcpy(bytes,"a\0bc",4);Event("http_body");return true;}
};
class Network:public StubGnuISteamNetworkingMessages002{public:
 EResult SendMessageToUser(const SteamNetworkingIdentity& id,const void* bytes,uint32 size,int flags,int channel)override{
  if(id.m_eType!=k_ESteamNetworkingIdentityType_SteamID||id.data.m_steamID64!=42||size!=3||memcmp(bytes,"a\0b",3)||flags!=8||channel!=2)abort();Event("network_message");return k_EResultOK;}
};
static Inventory inventory;static UGC ugc;static HTTP http;static Network network;
class Client:public StubISteamClient{public:
 void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn)override{WarningHook(fn);}
 void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t fn)override{Event(fn&&fn(1101)==1?"callback_filter_all":"callback_filter_none");}
 HSteamPipe CreateSteamPipe()override{Event("pipe_create");return 123;}
 bool BReleaseSteamPipe(HSteamPipe p)override{if(p!=123&&p!=124)abort();Event("pipe_release");return true;}
 HSteamUser CreateLocalUser(HSteamPipe* pipe,EAccountType type)override{if(!pipe||type!=k_EAccountTypeIndividual)abort();*pipe=124;Event("local_user_create");return 457;}
 HSteamUser ConnectToGlobalUser(HSteamPipe p)override{if(p!=123)abort();Event("user_connect");return 456;}
 void ReleaseUser(HSteamPipe p,HSteamUser u)override{if(!((p==123&&u==456)||(p==124&&u==457)))abort();Event("user_release");}
 ISteamUser* GetISteamUser(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();if(!strcmp(version,"SteamUser017"))return reinterpret_cast<ISteamUser*>(&legacyUser);if(!strcmp(version,"SteamUser023"))return reinterpret_cast<ISteamUser*>(&modernUser);return !strcmp(version,"SteamUser021")?reinterpret_cast<ISteamUser*>(&user):nullptr;}
 ISteamRemoteStorage* GetISteamRemoteStorage(HSteamUser u,HSteamPipe p,const char* v)override{if(u!=456||p!=123)abort();return !strcmp(v,"STEAMREMOTESTORAGE_INTERFACE_VERSION014")?reinterpret_cast<ISteamRemoteStorage*>(&storage):nullptr;}
 ISteamApps* GetISteamApps(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();if(!strcmp(version,"STEAMAPPS_INTERFACE_VERSION006"))return reinterpret_cast<ISteamApps*>(&legacyApps);return !strcmp(version,"STEAMAPPS_INTERFACE_VERSION008")?&apps:nullptr;}
 ISteamUtils* GetISteamUtils(HSteamPipe p,const char* version)override{if(p!=123)abort();return !strcmp(version,"SteamUtils007")?reinterpret_cast<ISteamUtils*>(&legacyUtils):nullptr;}
 void* GetISteamGenericInterface(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();
  if(!strcmp(version,"STEAMINVENTORY_INTERFACE_V003"))return &inventory;
  if(!strcmp(version,"STEAMUGC_INTERFACE_VERSION021"))return &ugc;
  if(!strcmp(version,"STEAMHTTP_INTERFACE_VERSION003"))return &http;
  if(!strcmp(version,"SteamNetworkingMessages002"))return &network;
  return !strcmp(version,"STEAMAPPTICKET_INTERFACE_VERSION001")?&appTicket:nullptr;}
 ISteamUserStats* GetISteamUserStats(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();return !strcmp(version,"STEAMUSERSTATS_INTERFACE_VERSION012")?&stats:nullptr;}
};
static Client client;
class LegacyClient:public StubISteamClient014{public:
 void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn)override{WarningHook(fn);}
 HSteamPipe CreateSteamPipe()override{return client.CreateSteamPipe();}
 bool BReleaseSteamPipe(HSteamPipe p)override{return client.BReleaseSteamPipe(p);}
 HSteamUser ConnectToGlobalUser(HSteamPipe p)override{return client.ConnectToGlobalUser(p);}
 void ReleaseUser(HSteamPipe p,HSteamUser u)override{client.ReleaseUser(p,u);}
 ISteamUser* GetISteamUser(HSteamUser u,HSteamPipe p,const char* v)override{return client.GetISteamUser(u,p,v);}
 ISteamUtils* GetISteamUtils(HSteamPipe p,const char* v)override{return client.GetISteamUtils(p,v);}
 ISteamApps* GetISteamApps(HSteamUser u,HSteamPipe p,const char* v)override{return client.GetISteamApps(u,p,v);}
 void* GetISteamGenericInterface(HSteamUser u,HSteamPipe p,const char* v)override{return client.GetISteamGenericInterface(u,p,v);}
};
static LegacyClient legacyClient;
class WarningClient017:public StubISteamClient017{public:
 HSteamPipe CreateSteamPipe()override{return client.CreateSteamPipe();}
 HSteamUser ConnectToGlobalUser(HSteamPipe p)override{return client.ConnectToGlobalUser(p);}
 void SetWarningMessageHook(SteamAPIWarningMessageHook_t fn)override{WarningHook(fn);}
};
static WarningClient017 warningClient017;
class ModernClient:public StubISteamClient023{public:
 HSteamPipe CreateSteamPipe()override{return client.CreateSteamPipe();}
 bool BReleaseSteamPipe(HSteamPipe p)override{return client.BReleaseSteamPipe(p);}
 HSteamUser ConnectToGlobalUser(HSteamPipe p)override{return client.ConnectToGlobalUser(p);}
 void ReleaseUser(HSteamPipe p,HSteamUser u)override{client.ReleaseUser(p,u);}
 ISteamUser* GetISteamUser(HSteamUser u,HSteamPipe p,const char* v)override{return client.GetISteamUser(u,p,v);}
};
static ModernClient modernClient;

extern "C" void* CreateInterface(const char* name,int*){if(!strcmp(name,"SteamClient023"))return &modernClient;if(!strcmp(name,"SteamClient017"))return &warningClient017;if(!strcmp(name,"SteamClient014"))return &legacyClient;return !strcmp(name,"SteamClient020")?&client:nullptr;}
extern "C" int8 Steam_BGetCallback(int pipe,CallbackMsg_t* message,int*){
 if(pipe!=123)abort();if(!callback)return false;
 static GetAuthSessionTicketResponse_t ticket;static UserStatsReceived_t received;
 message->m_hSteamUser=456;message->m_iCallback=callback;
 if(callback==163){ticket.m_hAuthTicket=987;ticket.m_eResult=k_EResultOK;message->m_pubParam=(uint8*)&ticket;message->m_cubParam=sizeof(ticket);}
 else if(callback==1040044){static uint8 data[784]{};message->m_pubParam=data;message->m_cubParam=sizeof(data);}
 else {received.m_nGameID=480;received.m_eResult=k_EResultOK;received.m_steamIDUser=CSteamID((uint64)76561198000000042ull);message->m_pubParam=(uint8*)&received;message->m_cubParam=sizeof(received);}
 return true;
}
extern "C" int8 Steam_FreeLastCallback(int pipe){if(pipe!=123)abort();callback=0;Event("callback_free");return true;}
extern "C" int8 Steam_GetAPICallResult(int,uint64,void*,int,int,int8*){return false;}
extern "C" void Steam_ReleaseThreadLocalMemory(int){Event("memory_release");}
"""
SOURCE_STUB = r"""
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
bool SteamOhosVerifyClientSource(const char* library,void* factory,char* report,size_t size){
 (void)library;(void)factory;(void)report;(void)size;
 const char* fail=getenv("MOCK_SOURCE_FAIL");return !fail||strcmp(fail,"1");
}
"""
@unittest.skipUnless(os.environ.get("STEAM_BRIDGE_REFERENCE") and os.environ.get("STEAM_BRIDGE_MINGW"), "pinned SDK / MinGW inputs required")
class RemoteBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="steam-bridge-host-")
        cls.directory = Path(cls.temp.name)
        cls.generated = cls.directory / "generated"
        reference = Path(os.environ["STEAM_BRIDGE_REFERENCE"])
        mingw = Path(os.environ["STEAM_BRIDGE_MINGW"])
        subprocess.run(["python3", str(ROOT / "scripts/generate_linux_steam_bridge.py"), "--reference", str(reference),
                        "--clang", str(mingw / "x86_64-w64-mingw32-clang++"), "--output", str(cls.generated)], check=True, capture_output=True)
        cls.catalog = json.loads((cls.generated / "abi-catalog.json").read_text())
        flags = ["-std=c++17", "-O1", "-w", "-I" + str(cls.generated), "-isystem", str(reference / "lsteamclient/steamworks_sdk_150")]
        mock = cls.directory / "mock.cpp"
        mock.write_text(MOCK)
        cls.root = cls.directory / "runtime"
        (cls.root / "steamrtarm64").mkdir(parents=True)
        subprocess.run(["g++", *flags, "-fPIC", "-shared", str(mock), "-o", str(cls.root / "steamrtarm64/steamclient.so")], check=True)
        stub = cls.directory / "source.c"
        stub.write_text(SOURCE_STUB)
        obj = cls.directory / "source.o"
        subprocess.run(["gcc", "-std=c11", "-c", str(stub), "-o", str(obj)], check=True)
        cls.server = cls.directory / "server"
        subprocess.run(["g++", *flags, str(ROOT / "entry/src/main/cpp/linux_steam/steam_bridge/server.cpp"), str(obj), "-ldl", "-o", str(cls.server)], check=True)

    def test_pe_unregistered_callback_release_false_is_not_a_transport_failure(self):
        # Compile the real PE callback pump as a host harness. The official
        # native release result can be false, while the RPC itself succeeded.
        source = (ROOT / "entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp").read_text()
        begin = source.index('extern "C" __declspec(dllexport) bool __cdecl Steam_BGetCallback')
        end = source.index('extern "C" __declspec(dllexport) bool __cdecl Steam_FreeLastCallback', begin)
        pump = source[begin:end].replace('__declspec(dllexport)', '').replace('__cdecl', '')
        harness = self.directory / "pe_callback_pump.cpp"
        harness.write_text(r'''#include "sdk.h"
#include "wire.h"
#include "callbacks.h"
#include <mutex>
#include <map>
#include <stdexcept>
using namespace hmosbridge;
static std::recursive_mutex mutex;
static std::map<int,std::vector<unsigned char>> callbacks;
static bool registered=false,repeat=false;
static SteamAPI_CheckCallbackRegistered_t callbackCheck=nullptr;
static unsigned polls=0,releases=0;
static void DispatchWarningMessages(int){}
static uint32 Check(int){return registered?1:0;}
[[noreturn]] static void Failed(uint32_t,Status){throw std::runtime_error("unsupported");}
static Status Exchange(uint32_t,uint32_t method,Buffer& in,Buffer& out){
 if(in.Get()!=123)return Invalid;in.End();
 if(method==1){
  bool received=repeat||++polls==1;out.Put(received);
  if(received){out.Put(456);out.Put(1040044);out.Put(0);out.Put(false);}
 }else if(method==2){++releases;out.Put(false);}else return Invalid;
 return Ok;
}
''' + pump + r'''
int main(){
 CallbackMsg_t message{};
 callbackCheck=Check;
 if(Steam_BGetCallback(123,&message,nullptr)||polls!=2||releases!=1)return 1;
 registered=true;polls=0;releases=0;
 try {Steam_BGetCallback(123,&message,nullptr);return 2;}catch(const std::runtime_error&){}
 if(releases)return 3;
 registered=false;callbackCheck=nullptr;polls=0;
 try {Steam_BGetCallback(123,&message,nullptr);return 4;}catch(const std::runtime_error&){}
 if(releases)return 5;
 callbackCheck=Check;repeat=true;polls=0;releases=0;
 if(Steam_BGetCallback(123,&message,nullptr)||releases!=64)return 6;
}
''')
        binary = self.directory / "pe_callback_pump"
        sdk = Path(os.environ["STEAM_BRIDGE_REFERENCE"]) / "lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++", "-std=c++17", "-O2", "-DNDEBUG", "-w", "-I" + str(self.generated),
                        "-I" + str(ROOT / "entry/src/main/cpp/linux_steam/steam_bridge"),
                        "-isystem", str(sdk), str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def generic(self, version):
        self.start();self.initialized()
        result=self.number(self.client,"GetISteamGenericInterface",Q(456)+Q(123)+text(version))
        self.assertTrue(result)
        return result

    def test_local_user_tracks_official_pipe_and_disconnect_cleanup(self):
        self.start();status,data=self.call(0,0,text('SteamClient020'));self.assertEqual(status,0)
        client=struct.unpack('!Q',data)[0];method=self.method('SteamClient020','CreateLocalUser')
        self.assertEqual(self.call(client,method,Q(0)+Q(1)),(2,b''))
        self.assertEqual(self.call(client,method,Q(1)+Q(1)),(0,Q(457)+Q(124)))
        self.assertEqual(self.call(client,self.method('SteamClient020','ReleaseUser'),Q(124)+Q(456)),(2,b''))
        events=self.close_and_events();self.assertEqual(events,['local_user_create','user_release','pipe_release'])

    def test_local_user_source_failure_releases_new_native_handles(self):
        self.start(source_fail=True);status,data=self.call(0,0,text('SteamClient020'));self.assertEqual(status,0)
        client=struct.unpack('!Q',data)[0]
        self.assertEqual(self.call(client,self.method('SteamClient020','CreateLocalUser'),Q(1)+Q(1)),(4,b''))
        self.assertEqual(self.close_and_events(),['local_user_create','user_release','pipe_release'])

    def test_all_upstream_versions_have_distinct_reviewed_vtable_slots(self):
        reference=Path(os.environ['STEAM_BRIDGE_REFERENCE'])/'lsteamclient'
        upstream={p.stem.split('_',1)[1] for p in reference.glob('cppISteam*_*.cpp')}
        self.assertEqual({m['interface'] for m in self.catalog['methods']},upstream)
        self.assertEqual(len(upstream),212)
        html=[m for m in self.catalog['methods'] if m['class'].startswith('ISteamHTMLSurface') and m['method']=='Init']
        self.assertEqual(len(html),5)
        self.assertTrue(all(m['windowsVtableSlot']==1 and m['gnuVtableSlot']==2 for m in html))
        sys.path.insert(0, str(ROOT / 'scripts'))
        from audit_linux_steam_bridge import verify_windows_vtables
        families={p.stem.split('_',1)[1]:p.stem[3:].split('_',1)[0]
                  for p in reference.glob('cppISteam*_*.cpp')}
        self.assertEqual(verify_windows_vtables(self.catalog,families,reference.parent),212)

    def test_inventory_capacity_queries_arrays_and_mismatched_extent(self):
        v='STEAMINVENTORY_INTERFACE_V003';obj=self.generic(v)
        method=self.method(v,'GetItemDefinitionIDs')
        # null buffer, zero span, present in/out capacity pointer.
        self.assertEqual(self.call(obj,method,Q(0)+Q(0)+Q(1)+Q(0)),(0,Q(1)+Q(3)))
        self.assertEqual(self.call(obj,method,Q(1)+Q(2)+Q(1)+Q(2)),(0,Q(0)+Q(0)+Q(3)))
        self.assertEqual(self.call(obj,method,Q(1)+Q(3)+Q(1)+Q(3)),(0,Q(1)+Q(3)+Q(11)+Q(22)+Q(33)+Q(3)))
        self.assertEqual(self.call(obj,method,Q(1)+Q(1)+Q(1)+Q(3)),(2,b''))
        generate=self.method(v,'GenerateItems')
        arrays=Q(1)+Q(1)+Q(2)+Q(11)+Q(22)+Q(1)+Q(2)+Q(1)+Q(2)+Q(2)
        self.assertEqual(self.call(obj,generate,arrays),(0,Q(1)+Q(77)))
        events=self.close_and_events()
        self.assertEqual(events.count('inventory_ids'),3)
        self.assertEqual(events.count('inventory_generate'),1)

    def test_ugc_string_arrays_preserve_null_empty_and_owned_strings(self):
        v='STEAMUGC_INTERFACE_VERSION021';obj=self.generic(v);method=self.method(v,'SetItemTags')
        self.assertEqual(self.call(obj,method,Q(91)+Q(0)+Q(0)),(0,Q(0)))
        self.assertEqual(self.call(obj,method,Q(91)+Q(1)+Q(0)+Q(0)),(0,Q(1)))
        self.assertEqual(self.call(obj,method,Q(91)+Q(1)+Q(2)+text('one')+text('two')+Q(0)),(0,Q(1)))
        for bad in [Q(4097),Q(1)+Q(2**64-1),Q(1)+Q(16385),Q(2)+text('one')]:
            self.assertEqual(self.call(obj,method,Q(91)+Q(1)+bad),(2,b''))
        self.assertEqual(self.close_and_events().count('ugc_tags'),3)

    def test_http_and_network_const_reference_use_native_objects(self):
        obj=self.generic('STEAMHTTP_INTERFACE_VERSION003')
        self.assertEqual(self.call(obj,self.method('STEAMHTTP_INTERFACE_VERSION003','GetHTTPResponseBodyData'),Q(3)+Q(1)+Q(4)+Q(4)),(0,Q(1)+Q(4)+b'a\0bc'))
        network=self.number(self.client,'GetISteamGenericInterface',Q(456)+Q(123)+text('SteamNetworkingMessages002'))
        self.assertTrue(network)
        send=self.method('SteamNetworkingMessages002','SendMessageToUser')
        identity=Q(16)+Q(8)+Q(42)
        self.assertEqual(self.call(network,send,identity+Q(1)+Q(3)+b'a\0b'+Q(3)+Q(8)+Q(2)),(0,Q(1)))
        self.assertEqual(self.call(network,send,identity+Q(1)+Q(2)+b'a\0'+Q(3)+Q(8)+Q(2)),(2,b''))
        usage=self.method('SteamUser021','TrackAppUsageEvent')
        self.assertEqual(self.call(self.user,usage,Q(480)+Q(17)+text('abi')),(0,b''))
        events=self.close_and_events()
        self.assertEqual(events.count('network_message'),1)
        self.assertEqual(events.count('gnu_gameid_reference'),1)

    def test_pointer_objects_string_arrays_and_capacity_use_production_codecs(self):
        pe=(ROOT/'entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp').read_text()
        body=pe[pe.index('template<class T> void WriteArgument'):pe.index('template<bool Outputs,class R,class... Args> static R Invoke')]
        source=self.directory/'extended_roundtrip.cpp'
        source.write_text('#define main BridgeServerEntry\n#include "'+str(ROOT/'entry/src/main/cpp/linux_steam/steam_bridge/server.cpp')+'"\n#undef main\nnamespace hmosbridge {\n'+body+r'''
}
using namespace hmosbridge;
int main(){
 const char* values[]={"one","two"};SteamParamStringArray_t array{values,2};
 Buffer encoded;WriteArgument(encoded,&array);ReadArgument<SteamParamStringArray_t*> native(encoded);encoded.End();
 auto* result=native.Get();if(!result||result->m_nNumStrings!=2||strcmp(result->m_ppStrings[1],"two"))return 1;
 Buffer empty;SteamParamStringArray_t zero{nullptr,0};WriteArgument(empty,&zero);ReadArgument<const SteamParamStringArray_t*> zeroArg(empty);empty.End();if(!zeroArg.Get()||zeroArg.Get()->m_nNumStrings)return 2;
 Buffer absent;WriteArgument(absent,static_cast<SteamParamStringArray_t*>(nullptr));ReadArgument<SteamParamStringArray_t*> missing(absent);absent.End();if(missing.Get())return 3;
 array.m_nNumStrings=4097;try{Buffer b;WriteArgument(b,&array);return 4;}catch(const WireError&){}
 array.m_nNumStrings=1;values[0]=nullptr;try{Buffer b;WriteArgument(b,&array);return 5;}catch(const WireError&){}
 std::string large(MaxString+1,'x');values[0]=large.c_str();try{Buffer b;WriteArgument(b,&array);return 6;}catch(const WireError&){}
 uint32 capacity=0xdeadbeef;Buffer query;WriteArgument(query,InOutValue(&capacity,false));ReadArgument<InOut<uint32>> size(query);query.End();if(!size.Get()||*size.Get())return 7;*size.Get()=17;Buffer reply;size.Output(reply);ReadOutput(reply,InOutValue(&capacity));reply.End();if(capacity!=17)return 8;
 SteamNetworkingIPAddr address{};address.ip.m_ipv6[15]=1;address.m_port=27015;
 const auto* input=&address;Buffer ip;WriteArgument(ip,input);ReadArgument<const SteamNetworkingIPAddr*> ipArg(ip);ip.End();if(!ipArg.Get()||ipArg.Get()->m_port!=27015||ipArg.Get()->ip.m_ipv6[15]!=1)return 9;
 for(auto type:{k_ESteamIPTypeIPv4,k_ESteamIPTypeIPv6}){
  SteamIPAddress_t a{};a.m_eType=type;if(type==k_ESteamIPTypeIPv4)a.x.m_unIPv4=0x12345678;else a.x.m_rgubIPv6[15]=42;
  Buffer b;PutValue(b,a);auto out=GetValue<SteamIPAddress_t>(b);b.End();if(out.m_eType!=type)return 10;
  if(type==k_ESteamIPTypeIPv4?out.x.m_unIPv4!=0x12345678:out.x.m_rgubIPv6[15]!=42)return 11;
 }
 SteamIPAddress_t invalid{};invalid.m_eType=static_cast<ESteamIPType>(7);try{Buffer b;PutValue(b,invalid);return 12;}catch(const WireError&){}
 SteamNetworkPingLocation_t location{};Buffer ref;WriteArgument(ref,OutputReference(location));ReadArgument<OutputReferenceValue<SteamNetworkPingLocation_t>> output(ref);ref.End();Buffer out;output.Output(out);ReadOutput(out,OutputReference(location));out.End();
}
''')
        sdk=Path(os.environ['STEAM_BRIDGE_REFERENCE'])/'lsteamclient/steamworks_sdk_150'
        binary=self.directory/'extended_roundtrip'
        subprocess.run(['g++','-std=c++17','-O1','-w','-I'+str(self.generated),'-I'+str(ROOT/'entry/src/main/cpp/linux_steam/steam_bridge'),'-isystem',str(sdk),str(source),str(self.directory/'source.o'),'-ldl','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

    def test_pe_ownership_ticket_preserves_blob_offsets_and_validates_bounds(self):
        source = (ROOT / 'entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp').read_text()
        body = source[source.index('static uint32 AppOwnershipTicket('):source.index('template<class R,class Capacity,class... Prefix>')]
        harness = self.directory / 'ownership_pe.cpp'
        harness.write_text(r'''#include "sdk.h"
#include "wire.h"
#include <mutex>
using namespace hmosbridge;
static std::recursive_mutex mutex;
static bool malformed=false;
[[noreturn]] static void Failed(uint32_t,Status){throw WireError();}
static Status Exchange(uint32_t object,uint32_t method,Buffer& in,Buffer& out){
 if(object!=19||method!=20||in.Get()!=480)return Invalid;
 uint64 cap=in.Get();bool buffer=GetScalar<bool>(in),flags[4];
 for(auto& flag:flags)flag=GetScalar<bool>(in);in.End();
 out.Put(24);uint32 offsets[]={4,8,16,8};
 for(unsigned i=0;i<4;++i)if(flags[i])out.Put(offsets[i]);
 uint64 copied=buffer&&cap>=24?24:0;out.Put(malformed?cap+1:copied);
 for(unsigned i=0;i<copied;++i){unsigned char value=(unsigned char)i;out.Bytes(&value,1);}
 return Ok;
}
''' + body + r'''
int main(){
 unsigned char storage[28];memset(storage,0xcc,sizeof(storage));uint32 a=0,u=0,s=0,n=0;
 if(AppOwnershipTicket(19,20,480,storage+2,24,&a,&u,&s,&n)!=24||a!=4||u!=8||s!=16||n!=8)return 1;
 for(unsigned i=0;i<24;++i)if(storage[i+2]!=i)return 2;
 if(storage[0]!=0xcc||storage[1]!=0xcc||storage[26]!=0xcc||storage[27]!=0xcc)return 3;
 if(AppOwnershipTicket(19,20,480,nullptr,0,nullptr,&u,nullptr,&n)!=24||u!=8||n!=8)return 4;
 memset(storage,0xcc,sizeof(storage));
 if(AppOwnershipTicket(19,20,480,storage,8,&a,&u,&s,&n)!=24||storage[0]!=0xcc)return 5;
 try{AppOwnershipTicket(19,20,480,nullptr,1,&a,&u,&s,&n);return 6;}catch(const WireError&){}
 try{AppOwnershipTicket(19,20,480,storage,65537,&a,&u,&s,&n);return 7;}catch(const WireError&){}
 malformed=true;
 try{AppOwnershipTicket(19,20,480,storage,24,&a,&u,&s,&n);return 8;}catch(const WireError&){}
}
''')
        binary = self.directory / 'ownership_pe'
        sdk = Path(os.environ['STEAM_BRIDGE_REFERENCE']) / 'lsteamclient/steamworks_sdk_150'
        subprocess.run(['g++', '-std=c++17', '-O2', '-DNDEBUG', '-w', '-I'+str(self.generated),
                        '-I'+str(ROOT / 'entry/src/main/cpp/linux_steam/steam_bridge'), '-isystem', str(sdk), str(harness), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_pe_framing_handles_partial_io_without_a_standalone_length_send(self):
        # Exercise the production Exchange/Transfer bodies with deterministic
        # short I/O. No timer threshold or mocked Steam authentication success.
        source = (ROOT / "entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp").read_text()
        transfer = source[source.index("static bool Transfer("):source.index("static bool Configure(")]
        exchange = source[source.index("static Status Exchange("):source.index("static void RegisterWarningHook(")]
        harness = self.directory / "pe_transport.cpp"
        harness.write_text(r'''#include "sdk.h"
#include "wire.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cstdio>
using namespace hmosbridge;
static int connection=1;constexpr int INVALID_SOCKET=-1;
static uint32_t sequence=0,appId=480;static unsigned char nonce[32]{};
static std::vector<unsigned char> sent,reply;static size_t cursor=0,chunk=1000000;
static unsigned sends=0;static size_t firstSend=0;
static bool Configure(){return connection!=INVALID_SOCKET;}
static void Report(const char*,uint32_t,Status){}
static void closesocket(int){connection=INVALID_SOCKET;}
static int send(int,const char* data,int count,int){
 if(!sends++)firstSend=count;size_t n=std::min((size_t)count,chunk);
 sent.insert(sent.end(),data,data+n);return (int)n;
}
static int recv(int,char* data,int count,int){
 size_t n=std::min({(size_t)count,reply.size()-cursor,chunk});
 if(n)memcpy(data,reply.data()+cursor,n);cursor+=n;return (int)n;
}
''' + transfer + exchange + r'''
static void Prepare(uint64_t responseSequence=1){
 connection=1;sequence=0;sent.clear();cursor=0;sends=0;
 Buffer response;response.Put(0x484d534252);response.Put(responseSequence);response.Put(Ok);response.Put(42);reply=response.Packet();
}
int main(){
 Buffer payload;payload.Put(123);
 for(size_t n:{size_t(1000000),size_t(3)}){
  Prepare();chunk=n;Buffer out;
  if(Exchange(0,1,payload,out)!=Ok||out.Get()!=42)return 1;out.End();
  Buffer expected;expected.Put(0x484d534231);expected.Put(1);expected.Put(0);expected.Put(1);expected.Put(480);expected.Bytes(nonce,32);expected.Put(123);
  if(sent!=expected.Packet()||firstSend!=sent.size())return 2;
  if(n==1000000&&sends!=1)return 3;
 }
 Prepare(99);Buffer out;if(Exchange(0,1,payload,out)!=Disconnected||connection!=INVALID_SOCKET)return 4;
 Prepare();reply.resize(6);Buffer truncated;if(Exchange(0,1,payload,truncated)!=Disconnected)return 5;
 Prepare();reply[0]=0x7f;Buffer oversized;if(Exchange(0,1,payload,oversized)!=Disconnected)return 6;
 Buffer largest;largest.data.resize(MaxFrame);auto packet=largest.Packet();
 if(packet.size()!=MaxFrame+4||packet[0]!=0||packet[1]!=16||packet[2]||packet[3])return 7;
 largest.data.push_back(0);try{largest.Packet();return 8;}catch(const WireError&){}
}
''')
        binary = self.directory / "pe_transport"
        sdk = Path(os.environ["STEAM_BRIDGE_REFERENCE"]) / "lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++", "-std=c++17", "-O2", "-w", "-I" + str(self.generated),
                        "-I" + str(ROOT / "entry/src/main/cpp/linux_steam/steam_bridge"),
                        "-isystem", str(sdk), str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_msvc_record_returns_use_the_caller_buffer(self):
        # Call the generated vtable using the explicit result-buffer ABI used
        # by Proton/MSVC. Guard words catch writing into the wrong location.
        source = self.directory / "record_return.cpp"
        source.write_text(r'''#include "sdk.h"
#include "wire.h"
#include <map>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <cstdint>
namespace hmosbridge {
static std::map<std::pair<std::string,uint32_t>,void*> interfaces;
static std::map<std::string,size_t> methodCounts;
static uint32_t lastMethod;
void* Wrap(const char*,uint32_t);
static void* Interface(const char* version,uint32_t object){return Wrap(version,object);}
template<class T>static void* MakeRemote(const char* version,uint32_t object,size_t methods){methodCounts[version]=methods;return new T(object);}
[[noreturn]] static void Failed(uint32_t,int){throw std::runtime_error("unsupported");}
template<bool,class R,class... A> static R Invoke(uint32_t,uint32_t method,A...){
 lastMethod=method;
 if constexpr(std::is_same_v<R,CSteamID>)return CSteamID((uint64)0x011000010000002aull);
 else if constexpr(std::is_same_v<R,SteamIPAddress_t>){SteamIPAddress_t value{};value.m_eType=k_ESteamIPTypeIPv6;value.x.m_rgubIPv6[15]=42;return value;}
 else if constexpr(std::is_same_v<R,bool>)return true;
 else if constexpr(std::is_void_v<R>)return;
 else return {};
}
template<class... A>static void RegisterCallbackCheck(A...){ }
template<class... A>static void RegisterWarningHook(A...){ }
template<class... A>static bool InterfaceCallResult(A...){return false;}
template<class R,class... I>static R Ticket(uint32_t,uint32_t,void*,int,uint32*,I...){return {};}
template<class... A>static uint32 AppOwnershipTicket(A...){return 0;}
template<class R,class... A>static R PathOutput(uint32_t,uint32_t,A...){return {};}
#include "pe_methods.inc"
}
int main(){
 struct Guarded {uint64 before;CSteamID id;uint64 after;};
 for(const auto* version:{"SteamUser021","SteamUser017","SteamUser023","STEAMAPPS_INTERFACE_VERSION008"}){
  void* object=hmosbridge::Wrap(version,1);if(!object)return 1;
  const unsigned slot=version[5]=='U'?2:17;
  auto** table=*reinterpret_cast<void***>(object);
  using Call=CSteamID*(*)(void*,CSteamID*);
  auto call=reinterpret_cast<Call>(table[slot]);
  Guarded result{0x12345678,CSteamID((uint64)0),0x76543210};
  for(unsigned i=0;i<4;++i){
   if(call(object,&result.id)!=&result.id)return 2;
   if(result.id.ConvertToUint64()!=0x011000010000002aull)return 3;
   if(result.before!=0x12345678||result.after!=0x76543210)return 4;
  }
 }
}
''')
        # Resolve the Apps vtable slot from the pinned SDK order.
        apps = [x for x in self.catalog["methods"] if x["interface"] == "STEAMAPPS_INTERFACE_VERSION008"]
        slot = next(i for i,x in enumerate(apps) if x["method"] == "GetAppOwner")
        source.write_text(source.read_text().replace("?2:17", "?2:" + str(slot)))
        extra=[]
        for version in sorted({m['interface'] for m in self.catalog['methods'] if m['method']=='GetPublicIP' and m['signature']=='SteamIPAddress_t ()'}):
            method=next(m for m in self.catalog['methods'] if m['interface']==version and m['method']=='GetPublicIP')
            extra.append('''{void* object=hmosbridge::Wrap("%s",1);auto** table=*reinterpret_cast<void***>(object);
using Call=SteamIPAddress_t*(*)(void*,SteamIPAddress_t*);auto call=reinterpret_cast<Call>(table[%d]);
struct Guard{uint64 before;SteamIPAddress_t address;uint64 after;};Guard value{123,{},456};
if(call(object,&value.address)!=&value.address||value.before!=123||value.after!=456||value.address.m_eType!=k_ESteamIPTypeIPv6||value.address.x.m_rgubIPv6[15]!=42)return 20;}'''%(version,method['windowsVtableSlot']))
        for version in sorted({m['interface'] for m in self.catalog['methods'] if m['class'].startswith('ISteamHTMLSurface')}):
            methods=[m for m in self.catalog['methods'] if m['interface']==version]
            extra.append('''{void* object=hmosbridge::Wrap("%s",1);if(hmosbridge::methodCounts["%s"]!=%d)return 21;
auto** table=*reinterpret_cast<void***>(object);reinterpret_cast<void(*)(void*)>(table[0])(object);
if(!reinterpret_cast<bool(*)(void*)>(table[1])(object))return 22;}'''%(version,version,len(methods)+1))
        # MSVC groups overloads at their first declaration, in reverse order.
        # Invoke actual generated slots, not just their catalog metadata. A
        # null output is intentional: this checks dispatch without a fake RPC.
        for method in self.catalog['methods']:
            if method['method']=='GetStat' and method['signature'] in (
                    'bool (const char *, int32 *)','bool (const char *, float *)'):
                output='float' if 'float' in method['signature'] else 'int32'
                extra.append('''{void* object=hmosbridge::Wrap("%s",1);auto** table=*reinterpret_cast<void***>(object);
using Call=bool(*)(void*,const char*,%s*);hmosbridge::lastMethod=0;
if(!reinterpret_cast<Call>(table[%d])(object,"stat",nullptr)||hmosbridge::lastMethod!=%du)return 23;}'''
                             %(method['interface'],output,method['windowsVtableSlot'],method['id']))
            if method['interface']=='SteamFriends001' and method['method']=='SendMsgToFriend':
                binary=method['signature'].startswith('bool')
                arguments='CSteamID((uint64)42),k_EChatEntryTypeChatMsg,"x"'+(',1' if binary else '')
                signature='bool(*)(void*,CSteamID,EChatEntryType,const void*,int)' if binary else 'void(*)(void*,CSteamID,EChatEntryType,const char*)'
                extra.append('''{void* object=hmosbridge::Wrap("SteamFriends001",1);auto** table=*reinterpret_cast<void***>(object);
using Call=%s;hmosbridge::lastMethod=0;reinterpret_cast<Call>(table[%d])(object,%s);
if(hmosbridge::lastMethod!=%du)return 24;}'''%(signature,method['windowsVtableSlot'],arguments,method['id']))
        for version,layout in self.catalog['interfaceLayouts'].items():
            extra.append('{hmosbridge::Wrap("%s",1);if(hmosbridge::methodCounts["%s"]!=%d)return 25;}'
                         %(version,version,layout['windowsSlots']))
        text=source.read_text();at=text.rfind('\n}');source.write_text(text[:at]+'\n'+'\n'.join(extra)+text[at:])
        binary = self.directory / "record_return"
        sdk = Path(os.environ["STEAM_BRIDGE_REFERENCE"]) / "lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++", "-std=c++17", "-O1", "-w", "-I" + str(self.generated), "-I" + str(ROOT / "entry/src/main/cpp/linux_steam/steam_bridge"),
                        "-isystem", str(sdk), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.endpoint = self.directory / "endpoint"
        self.endpoint.unlink(missing_ok=True)
        self.trace = self.directory / "trace"
        self.trace.unlink(missing_ok=True)
        self.child = None
        self.connection = None
        self.sequence = 0

    def tearDown(self):
        if self.connection:
            self.connection.close()
        if self.child:
            if self.child.poll() is None:
                self.child.terminate()
            self.child.communicate(timeout=3)

    def start(self, source_fail=False, private_callback=False, path_location='download'):
        environment=dict(os.environ)
        if private_callback: environment["MOCK_PRIVATE_CALLBACK"]="1"
        environment.update(STEAMOHOS_WINE_PREFIX='/fixture/prefix', STEAMOHOS_Z_ROOT='/fixture/download',
            MOCK_APP_PATH=('/fixture/prefix/drive_c' if path_location == 'private' else '/fixture/download') + '/SteamLinuxARM64/library/steamapps/common/Test Game',
            MOCK_USER_PATH='/fixture/prefix/drive_c/users/steamuser/Documents/SteamData')
        self.child = subprocess.Popen([str(self.server), "--endpoint", str(self.endpoint)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(environment, STEAMOHOS_CODE_ROOT=str(self.root), SteamAppId="480", STEAMOHOS_STEAMCLIENT_EXPECTED_PID=str(os.getpid()),
                     MOCK_TRACE=str(self.trace), MOCK_SOURCE_FAIL="1" if source_fail else "0"))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self.endpoint.exists():
                lines = self.endpoint.read_text().splitlines()
                if len(lines) == 4:
                    break
            time.sleep(0.005)
        else:
            self.fail("No endpoint")
        self.assertEqual(lines[0], "HMOSDECK-BRIDGE/1")
        self.assertEqual(self.endpoint.stat().st_mode & 0o777, 0o600)
        self.nonce = bytes.fromhex(lines[2])
        self.connection = socket.create_connection(("127.0.0.1", int(lines[1])), timeout=2)

    def receive(self, size):
        data = b""
        while len(data) < size:
            part = self.connection.recv(size - len(data))
            if not part:
                raise EOFError()
            data += part
        return data

    def call(self, object_id, method, payload=b"", app=480, nonce=None):
        self.sequence += 1
        frame = b"".join(Q(v) for v in [0x484d534231, self.sequence, object_id, method, app]) + (nonce or self.nonce) + payload
        self.connection.sendall(struct.pack("!I", len(frame)) + frame)
        size = struct.unpack("!I", self.receive(4))[0]
        data = self.receive(size)
        magic, sequence, status = struct.unpack("!QQQ", data[:24])
        self.assertEqual((magic, sequence), (0x484d534252, self.sequence))
        return status, data[24:]

    def method(self, version, name, suffix=None):
        matches = [m for m in self.catalog["methods"] if m["interface"] == version and m["method"] == name and (not suffix or suffix in m["signature"])]
        self.assertEqual(len(matches), 1)
        return matches[0]["id"]

    def number(self, object_id, name, payload=b"", version="SteamClient020", suffix=None):
        status, data = self.call(object_id, self.method(version, name, suffix), payload)
        self.assertEqual(status, 0)
        self.assertEqual(len(data), 8)
        return struct.unpack("!Q", data)[0]

    def initialized(self):
        status, data = self.call(0, 0, text("SteamClient020"))
        self.assertEqual(status, 0)
        self.client = struct.unpack("!Q", data)[0]
        self.assertTrue(self.client)
        self.assertEqual(self.number(self.client, "CreateSteamPipe"), 123)
        self.assertEqual(self.number(self.client, "ConnectToGlobalUser", Q(123)), 456)
        self.user = self.number(self.client, "GetISteamUser", Q(456) + Q(123) + text("SteamUser021"))
        self.assertTrue(self.user)

    def close_and_events(self):
        self.connection.close()
        self.connection = None
        stdout, stderr = self.child.communicate(timeout=3)
        self.assertNotIn(self.nonce.hex().encode(), stdout + stderr)
        self.assertNotIn(b"76561198000000042", stdout + stderr)
        return self.trace.read_text().splitlines()

    def check_directory_queries(self, location):
        self.start(path_location=location)
        self.initialized()
        apps = self.number(self.client, "GetISteamApps", Q(456) + Q(123) + text("STEAMAPPS_INTERFACE_VERSION008"))
        method = self.method("STEAMAPPS_INTERFACE_VERSION008", "GetAppInstallDir")
        folder = ('C:' if location == 'private' else 'Z:') + r'\SteamLinuxARM64\library\steamapps\common\Test Game'
        self.assertEqual(self.call(apps, method, Q(480) + Q(4096)), (0, Q(len(folder)) + text(folder)))
        self.assertEqual(self.call(apps, method, Q(480) + Q(0)), (0, Q(len(folder)) + text('')))
        self.assertEqual(self.call(apps, method, Q(480) + Q(4)), (0, Q(len(folder)) + text(folder[:3])))
        self.assertEqual(self.call(apps, method, Q(999) + Q(4096)), (0, Q(0) + text('')))
        self.assertEqual(self.call(apps, method, Q(480) + Q(16385)), (2, b''))
        method = self.method('SteamUser021', 'GetUserDataFolder')
        folder = r'C:\users\steamuser\Documents\SteamData'
        self.assertEqual(self.call(self.user, method, Q(4096)), (0, Q(1) + text(folder)))
        self.assertEqual(self.call(self.user, method, Q(4)), (0, Q(0) + text('')))
        self.close_and_events()

    def test_pe_binary_outputs_preserve_tail_and_reject_oversized_reply(self):
        pe=(ROOT/"entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp").read_text()
        body=pe[pe.index("template<class T> void WriteArgument"):pe.index("template<class R,class... Identity>static R Ticket")]
        source=self.directory/"pe_spans.cpp"
        source.write_text(r'''#include "sdk.h"
#include "wire.h"
#include <mutex>
#include <map>
using namespace hmosbridge;
static std::recursive_mutex mutex;
static std::map<std::pair<uint32_t,uint32_t>,std::string>strings;
static bool malformed=false;
[[noreturn]]static void Failed(uint32_t,Status){throw WireError();}
static Status Exchange(uint32_t object,uint32_t method,Buffer& in,Buffer& out){
 if(object!=1)return Invalid;
 if(method==11){if(in.Get()!=1||in.Get()!=3)return Invalid;char bytes[3];in.Read(bytes,3);if(memcmp(bytes,"a\0b",3)||in.Get()!=3)return Invalid;in.End();out.Put(1);return Ok;}
 if(method==12){if(in.Get()!=1||in.Get()!=16||in.Get()!=16)return Invalid;in.End();out.Put(3);out.Put(malformed?17:3);out.Bytes("a\0b",3);return Ok;}
 if(method==13){in.End();InputAnalogActionData_t v{};v.eMode=k_EInputSourceMode_JoystickMove;v.x=0.5f;v.y=-0.25f;v.bActive=true;PutValue(out,v);return Ok;}
 return Invalid;
}
''' + body + r'''
int main(){
 const char input[3]={'a',0,'b'};
 if(!Invoke<false,bool>(1,11,InputSpan(input,3),int32(3)))return 1;
 unsigned char bytes[20];memset(bytes,0xcc,sizeof(bytes));
 if(Invoke<false,int32>(1,12,OutputSpan<true>(bytes+2,16),int32(16))!=3)return 2;
 if(memcmp(bytes+2,input,3)||bytes[0]!=0xcc||bytes[1]!=0xcc)return 3;
 for(int i=5;i<20;++i)if(bytes[i]!=0xcc)return 4;
 auto value=Invoke<false,InputAnalogActionData_t>(1,13);
 if(value.eMode!=k_EInputSourceMode_JoystickMove||value.x!=0.5f||value.y!=-0.25f||!value.bActive)return 5;
 malformed=true;memset(bytes,0xcc,sizeof(bytes));
 try{Invoke<false,int32>(1,12,OutputSpan<true>(bytes+2,16),int32(16));return 6;}catch(const WireError&){}
 for(auto value:bytes)if(value!=0xcc)return 7;
}
''')
        binary=self.directory/"pe_spans"
        sdk=Path(os.environ["STEAM_BRIDGE_REFERENCE"])/"lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++","-std=c++17","-O2","-DNDEBUG","-I"+str(self.generated),"-I"+str(ROOT/"entry/src/main/cpp/linux_steam/steam_bridge"),"-isystem",str(sdk),str(source),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

    def test_modern_client_and_user_023_use_distinct_abi_and_real_ticket_cleanup(self):
        self.start()
        status,data=self.call(0,0,text("SteamClient023"))
        self.assertEqual(status,0)
        client=struct.unpack("!Q",data)[0]
        self.assertTrue(client)
        self.assertEqual(self.number(client,"CreateSteamPipe",version="SteamClient023"),123)
        self.assertEqual(self.number(client,"ConnectToGlobalUser",Q(123),version="SteamClient023"),456)
        user=self.number(client,"GetISteamUser",Q(456)+Q(123)+text("SteamUser023"),version="SteamClient023")
        self.assertTrue(user)
        self.assertEqual(self.number(user,"BLoggedOn",version="SteamUser023"),1)
        method=self.method("SteamUser023","GetAuthSessionTicket")
        self.assertEqual(self.call(user,method,Q(64)+Q(0)),(0,Q(991)+Q(3)+Q(3)+b"x\0z"))
        cancel=self.method("SteamUser023","CancelAuthTicket")
        self.assertEqual(self.call(user,cancel,Q(991)),(0,b""))
        # SteamID identity: enum 16, active payload size 8, real value 42.
        self.assertEqual(self.call(user,method,Q(64)+Q(1)+Q(16)+Q(8)+Q(42)),(0,Q(991)+Q(3)+Q(3)+b"x\0z"))
        self.assertEqual(self.call(user,method,Q(64)+Q(1)+Q(16)+Q(129)),(2,b""))
        self.assertEqual(self.number(user,"GetAuthTicketForWebApi",text("service"),version="SteamUser023"),992)
        events=self.close_and_events()
        self.assertEqual(events.count("modern_cancel"),3)
        self.assertEqual(events.count("modern_identity"),1)
        self.assertEqual(events[-2:],["user_release","pipe_release"])

    def test_binary_storage_spans_validate_original_extent_before_native_call(self):
        self.start()
        self.initialized()
        version="STEAMREMOTESTORAGE_INTERFACE_VERSION014"
        storage=self.number(self.client,"GetISteamRemoteStorage",Q(456)+Q(123)+text(version))
        self.assertTrue(storage)
        write=self.method(version,"FileWrite")
        read=self.method(version,"FileRead")
        self.assertEqual(self.call(storage,write,text("save")+Q(1)+Q(3)+b"a\0b"+Q(3)),(0,Q(1)))
        self.assertEqual(self.call(storage,read,text("save")+Q(1)+Q(16)+Q(16)),(0,Q(3)+Q(3)+b"a\0b"))
        # Declared span smaller than the native API capacity must never reach
        # the official method, even with authenticated framing.
        self.assertEqual(self.call(storage,read,text("save")+Q(1)+Q(3)+Q(16)),(2,b""))
        self.assertEqual(self.call(storage,write,text("save")+Q(1)+Q(3)+b"a\0b"+Q(16)),(2,b""))
        self.assertEqual(self.call(storage,read,text("save")+Q(1)+Q(2**32)+Q(16)),(2,b""))
        self.assertEqual(self.call(storage,write,text("save")+Q(0)+Q(3)+Q(3)),(2,b""))
        events=self.close_and_events()
        self.assertEqual(events.count("file_write"),1)
        self.assertEqual(events.count("file_read"),1)

    def test_sdk_callback_and_record_codecs_are_fieldwise_and_bounded(self):
        source=self.directory/"value_roundtrip.cpp"
        source.write_text(r'''#include "sdk.h"
#include "wire.h"
#include "callbacks.h"
using namespace hmosbridge;
int main(){
 static_assert(Record<FriendGameInfo_t>::supported);
 static_assert(!Record<SteamParamStringArray_t>::supported);
 static_assert(!Record<HTML_NeedsPaint_t>::supported);
 FriendGameInfo_t input{};input.m_gameID=CGameID((uint64)480);input.m_unGameIP=0x12345678;input.m_usGamePort=65535;input.m_usQueryPort=42;input.m_steamIDLobby=CSteamID((uint64)99);
 Buffer wire;PutValue(wire,input);if(wire.data.size()!=40)return 1;
 auto output=GetValue<FriendGameInfo_t>(wire);wire.End();
 if(output.m_gameID.ToUint64()!=480||output.m_unGameIP!=0x12345678||output.m_usGamePort!=65535||output.m_usQueryPort!=42||output.m_steamIDLobby.ConvertToUint64()!=99)return 2;
 GetTicketForWebApiResponse_t ticket{};ticket.m_hAuthTicket=92;ticket.m_eResult=k_EResultOK;ticket.m_cubTicket=3;ticket.m_rgubTicket[0]=42;
 std::vector<unsigned char> bytes;Buffer encoded;
 if(CallbackCodec(ticket.k_iCallback,&ticket,sizeof(ticket),encoded,true,bytes)!=Ok)return 3;
 if(CallbackCodec(ticket.k_iCallback,nullptr,0,encoded,false,bytes)!=Ok||bytes.size()!=sizeof(ticket))return 4;
 encoded.End();auto* restored=reinterpret_cast<const GetTicketForWebApiResponse_t*>(bytes.data());
 if(restored->m_hAuthTicket!=92||restored->m_eResult!=k_EResultOK||restored->m_cubTicket!=3||restored->m_rgubTicket[0]!=42)return 5;
 Buffer bad;if(CallbackCodec(ticket.k_iCallback,&ticket,sizeof(ticket)-1,bad,true,bytes)!=Invalid)return 6;
 SteamNetworkingIdentity identity{};identity.m_eType=k_ESteamNetworkingIdentityType_IPAddress;identity.m_cbSize=sizeof(SteamNetworkingIPAddr);identity.data.m_ip.ip.m_ipv6[15]=1;identity.data.m_ip.m_port=27015;
 Buffer net;IdentityField(net,true,identity);SteamNetworkingIdentity decoded{};IdentityField(net,false,decoded);net.End();
 if(decoded.data.m_ip.ip.m_ipv6[15]!=1||decoded.data.m_ip.m_port!=27015)return 7;
 try{OutputSpan(static_cast<char*>(nullptr),-1);return 8;}catch(const WireError&){}
 try{InputSpan(static_cast<const void*>(nullptr),1);return 9;}catch(const WireError&){}
 try{OutputSpan(static_cast<void*>(nullptr),MaxFrame);return 10;}catch(const WireError&){}
 if(CallbackSize(GlobalStatsReceived_t::k_iCallback)!=sizeof(GlobalStatsReceived_t))return 11;
}
''')
        binary=self.directory/"value_roundtrip"
        sdk=Path(os.environ["STEAM_BRIDGE_REFERENCE"])/"lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++","-std=c++17","-O2","-DNDEBUG","-I"+str(self.generated),"-I"+str(ROOT/"entry/src/main/cpp/linux_steam/steam_bridge"),"-isystem",str(sdk),str(source),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

    def test_legacy_client_014_uses_its_own_vtable_and_real_lifecycle(self):
        self.start()
        status, data = self.call(0, 0, text("SteamClient014"))
        self.assertEqual(status, 0)
        client = struct.unpack("!Q", data)[0]
        self.assertTrue(client)
        self.assertEqual(self.number(client, "CreateSteamPipe", version="SteamClient014"), 123)
        self.assertEqual(self.number(client, "ConnectToGlobalUser", Q(123), version="SteamClient014"), 456)
        user = self.number(client, "GetISteamUser", Q(456)+Q(123)+text("SteamUser021"), version="SteamClient014")
        self.assertTrue(user)
        self.assertEqual(self.number(user, "BLoggedOn", version="SteamUser021"), 1)
        self.assertEqual(self.number(client, "GetISteamUser", Q(456)+Q(123)+text("SteamUser999"), version="SteamClient014"), 0)
        self.assertEqual(self.call(0, 0, text("SteamClient999")), (0, Q(0)))
        self.assertEqual(self.call(client, self.method("SteamClient020", "CreateSteamPipe")), (2, b''))
        self.assertEqual(self.close_and_events()[-2:], ["user_release", "pipe_release"])

    def test_directory_queries_translate_public_paths_and_respect_buffers(self):
        self.check_directory_queries('download')

    def test_legacy_core_interfaces_keep_typed_vtables_and_ticket_cleanup(self):
        self.start()
        status, data = self.call(0, 0, text('SteamClient014'))
        self.assertEqual(status, 0)
        client = struct.unpack('!Q', data)[0]
        self.assertEqual(self.number(client, 'CreateSteamPipe', version='SteamClient014'), 123)
        self.assertEqual(self.number(client, 'ConnectToGlobalUser', Q(123), version='SteamClient014'), 456)
        utils = self.number(client, 'GetISteamUtils', Q(123)+text('SteamUtils007'), version='SteamClient014')
        self.assertTrue(utils)
        self.assertEqual(self.number(utils, 'GetAppID', version='SteamUtils007'), 480)
        self.assertEqual(self.call(utils, self.method('SteamUtils007', 'SetOverlayNotificationInset'), Q(7)+Q(9)), (0, b''))
        self.assertEqual(self.call(utils, self.method('SteamUtils010', 'GetAppID')), (2, b''))
        user = self.number(client, 'GetISteamUser', Q(456)+Q(123)+text('SteamUser017'), version='SteamClient014')
        self.assertTrue(user)
        self.assertEqual(self.number(user, 'GetSteamID', version='SteamUser017'), 76561198000000043)
        self.assertEqual(self.number(user, 'BLoggedOn', version='SteamUser017'), 1)
        apps = self.number(client, 'GetISteamApps', Q(456)+Q(123)+text('STEAMAPPS_INTERFACE_VERSION006'), version='SteamClient014')
        self.assertTrue(apps)
        self.assertEqual(self.number(apps, 'BIsSubscribedApp', Q(480), version='STEAMAPPS_INTERFACE_VERSION006'), 1)
        self.assertEqual(self.number(apps, 'BIsSubscribedApp', Q(999), version='STEAMAPPS_INTERFACE_VERSION006'), 0)
        self.assertEqual(self.call(user, self.method('SteamUser017', 'GetAuthSessionTicket'), Q(64)),
                         (0, Q(654)+Q(3)+Q(3)+b'\x5a'*3))
        events = self.close_and_events()
        self.assertEqual(events[-4:], ['legacy_ticket_create', 'legacy_ticket_cancel', 'user_release', 'pipe_release'])

    def test_ownership_ticket_queries_preserve_native_results_and_null_outputs(self):
        self.start()
        self.initialized()
        interface = 'STEAMAPPTICKET_INTERFACE_VERSION001'
        ticket = self.number(self.client, 'GetISteamGenericInterface', Q(456)+Q(123)+text(interface))
        self.assertTrue(ticket)
        method = self.method(interface, 'GetAppOwnershipTicketData')
        def request(app, cap, buffer, flags=(1,1,1,1)):
            return b''.join(Q(value) for value in (app,cap,buffer,*flags))
        metadata = Q(24)+Q(4)+Q(8)+Q(16)+Q(8)
        self.assertEqual(self.call(ticket, method, request(480,24,1)), (0,metadata+Q(24)+bytes(range(24))))
        self.assertEqual(self.call(ticket, method, request(480,8,1)), (0,metadata+Q(0)))
        self.assertEqual(self.call(ticket, method, request(480,0,0,(0,1,0,1))), (0,Q(24)+Q(8)+Q(8)+Q(0)))
        self.assertEqual(self.call(ticket, method, request(999,24,1)), (0,Q(0)*6))
        self.assertEqual(self.call(ticket, method, request(480,65537,1)), (2,b''))
        self.assertEqual(self.call(ticket, method, request(480,24,0)), (2,b''))
        self.assertEqual(self.call(ticket, method, request(480,24,1,(2,1,1,1))), (2,b''))
        self.assertEqual(self.close_and_events().count('ownership_ticket_called'), 4)

    def test_directory_queries_translate_private_C_drive_paths(self):
        self.check_directory_queries('private')

    def test_callback_registration_does_not_transmit_or_invoke_a_windows_pointer(self):
        self.start()
        self.initialized()
        method=self.method("SteamClient020", "Set_SteamAPI_CCheckCallbackRegisteredInProcess")
        self.assertEqual(self.call(self.client, method, Q(1)), (0, b""))
        self.assertEqual(self.call(self.client, method, Q(0)), (0, b""))
        self.assertEqual(self.call(self.client, method, Q(0x12345678)), (2, b""))
        events=self.close_and_events()
        self.assertIn("callback_filter_all", events)
        self.assertIn("callback_filter_none", events)

    def test_warning_hook_registers_native_callback_and_delivers_only_bounded_text(self):
        self.start()
        # Reproduce the exact legacy ABI/method recorded by GuJian, rather
        # than accidentally testing only the current SDK's vtable.
        status, data = self.call(0, 0, text('SteamClient017'))
        self.assertEqual(status, 0)
        self.client = struct.unpack('!Q', data)[0]
        self.assertEqual(self.number(self.client, 'CreateSteamPipe', version='SteamClient017'), 123)
        self.assertEqual(self.number(self.client, 'ConnectToGlobalUser', Q(123), version='SteamClient017'), 456)
        method = self.method('SteamClient017', 'SetWarningMessageHook')
        self.assertEqual(method, 1911970518)
        self.assertEqual(self.call(self.client, method, Q(1)), (0, b''))
        expected = Q(1) + Q(1) + text('official fixture warning')
        self.assertEqual(self.call(0, 5, Q(123)), (0, expected))
        self.assertEqual(self.call(0, 5, Q(123)), (0, Q(0)))
        self.assertEqual(self.call(self.client, method, Q(0x12345678)), (2, b''))
        self.assertEqual(self.call(self.client, method, Q(1) + Q(2)), (2, b''))
        self.assertEqual(self.call(self.client, method, Q(1)), (0, b''))
        self.assertEqual(self.call(self.client, method, Q(0)), (0, b''))
        self.assertEqual(self.call(0, 5, Q(123)), (0, Q(0)), 'Unregister must discard queued text')
        self.assertEqual(self.call(0, 5, Q(999)), (2, b''))
        events = self.close_and_events()
        self.assertEqual(events.count('warning_registered'), 2)
        self.assertGreaterEqual(events.count('warning_unregistered'), 2)

    def test_warning_hook_cleanup_precedes_owning_user_release(self):
        self.start()
        self.initialized()
        utils = self.number(self.client, 'GetISteamUtils', Q(123) + text('SteamUtils007'))
        method = self.method('SteamUtils007', 'SetWarningMessageHook')
        self.assertEqual(self.call(utils, method, Q(1)), (0, b''))
        events = self.close_and_events()
        self.assertLess(events.index('warning_unregistered'), events.index('user_release'))

    def test_pe_warning_hook_keeps_pointer_local_dispatches_and_allows_reentrant_api(self):
        source = (ROOT / 'entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp').read_text()
        begin = source.index('static void RegisterWarningHook(')
        end = source.index('template<class T> void WriteArgument', begin)
        harness = self.directory / 'pe_warning_hook.cpp'
        harness.write_text(r'''#include "sdk.h"
#include "warning_queue.h"
#include <stdexcept>
using namespace hmosbridge;
static std::recursive_mutex mutex;
static SteamAPIWarningMessageHook_t warningHook=nullptr;
static WarningQueue nativeWarnings;
static int delivered=0,polled=0;static bool malformed=false;
[[noreturn]] static void Failed(uint32_t,Status){throw std::runtime_error("invalid");}
static Status Exchange(uint32_t object,uint32_t method,Buffer& input,Buffer& output){
 if(method==99){if(object!=7)return Invalid;bool enabled=GetScalar<bool>(input);input.End();nativeWarnings.Reset(enabled);return Ok;}
 if(method!=WarningMessagesMethod||object||input.Get()!=123)return Invalid;input.End();++polled;
 if(malformed){output.Put(MaxWarningMessages+1);return Ok;}
 nativeWarnings.Drain(output);return Ok;
}
static void DispatchWarningMessages(int);
static void Hook(int level,const char* message){
 if(level!=1||strcmp(message,"official"))throw std::runtime_error("wrong text");
 ++delivered;DispatchWarningMessages(123);
}
''' + source[begin:end] + r'''
int main(){
 RegisterWarningHook(7,99,Hook);nativeWarnings.Push(1,"official");DispatchWarningMessages(123);
 if(delivered!=1||polled!=1)return 1;
 nativeWarnings.Push(1,"official");RegisterWarningHook(7,99,nullptr);DispatchWarningMessages(123);
 if(delivered!=1||polled!=1)return 2;
 RegisterWarningHook(7,99,Hook);malformed=true;
 try{DispatchWarningMessages(123);return 3;}catch(const std::runtime_error&){}
 malformed=false;nativeWarnings.Push(1,"official");DispatchWarningMessages(123);
 if(delivered!=2)return 4;
 WarningQueue queue;queue.Reset(true);std::string large(MaxWarningText+100,'x');
 for(unsigned i=0;i<MaxWarningMessages+5;++i)queue.Push(i,large.c_str());
 Buffer out;queue.Drain(out);if(out.Get()!=MaxWarningMessages)return 5;
 for(unsigned i=0;i<MaxWarningMessages;++i){if(GetScalar<int>(out)!=(int)i+5||out.String().size()!=MaxWarningText)return 6;}out.End();
}
''')
        binary = self.directory / 'pe_warning_hook'
        sdk = Path(os.environ['STEAM_BRIDGE_REFERENCE']) / 'lsteamclient/steamworks_sdk_150'
        subprocess.run(['g++', '-std=c++17', '-O2', '-w', '-I' + str(self.generated),
                        '-I' + str(ROOT / 'entry/src/main/cpp/linux_steam/steam_bridge'),
                        '-isystem', str(sdk), str(harness), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)

    def test_real_handle_lifecycle_and_typed_identity(self):
        self.start()
        self.initialized()
        self.assertEqual(self.number(self.user, "GetSteamID", version="SteamUser021"), 76561198000000042)
        self.assertEqual(self.number(self.user, "BLoggedOn", version="SteamUser021"), 1)
        apps = self.number(self.client, "GetISteamApps", Q(456) + Q(123) + text("STEAMAPPS_INTERFACE_VERSION008"))
        self.assertEqual(self.number(apps, "BIsSubscribedApp", Q(480), "STEAMAPPS_INTERFACE_VERSION008"), 1)
        self.assertEqual(self.number(apps, "BIsSubscribedApp", Q(999999), "STEAMAPPS_INTERFACE_VERSION008"), 0)
        self.assertEqual(self.close_and_events()[-2:], ["user_release", "pipe_release"])

    def test_source_failure_releases_objects_and_returns_explicit_error(self):
        self.start(source_fail=True)
        _, data = self.call(0, 0, text("SteamClient020"))
        client = struct.unpack("!Q", data)[0]
        self.number(client, "CreateSteamPipe")
        status, data = self.call(client, self.method("SteamClient020", "ConnectToGlobalUser"), Q(123))
        self.assertEqual((status, data), (4, b""))
        self.assertEqual(self.close_and_events()[-2:], ["user_release", "pipe_release"])

    def test_scalar_output_and_real_callback_fields(self):
        self.start()
        self.initialized()
        stats = self.number(self.client, "GetISteamUserStats", Q(456) + Q(123) + text("STEAMUSERSTATS_INTERFACE_VERSION012"))
        status, data = self.call(stats, self.method("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievement"), text("fixture") + Q(1))
        self.assertEqual((status, struct.unpack("!QQ", data)), (0, (1, 1)))
        self.assertEqual(self.number(stats, "RequestCurrentStats", version="STEAMUSERSTATS_INTERFACE_VERSION012"), 1)
        status, data = self.call(0, 1, Q(123))
        self.assertEqual(status, 0)
        self.assertEqual(struct.unpack("!QQQQQQQQ", data), (1, 456, 1101, 0, 1, 480, 1, 76561198000000042))
        self.assertEqual(self.call(0, 2, Q(123)), (0, Q(1)))
        self.assertEqual(self.call(0, 1, Q(123)), (0, Q(0)))
        self.assertIn("callback_free", self.close_and_events())

    def test_unknown_callback_only_returns_metadata_and_waits_for_pe_registration_decision(self):
        self.start(private_callback=True)
        self.initialized()
        stats = self.number(self.client, "GetISteamUserStats", Q(456) + Q(123) + text("STEAMUSERSTATS_INTERFACE_VERSION012"))
        self.number(stats, "RequestCurrentStats", version="STEAMUSERSTATS_INTERFACE_VERSION012")
        self.assertEqual(self.call(0, 1, Q(123)), (0, Q(1)+Q(456)+Q(1040044)+Q(0)+Q(0)))
        self.assertNotIn("callback_free", self.trace.read_text())
        # A pending callback cannot be overwritten by another request. The PE
        # owns the registered/unregistered decision and sends the real release.
        self.assertEqual(self.call(0, 1, Q(123)), (2, b""))
        self.assertEqual(self.call(0, 2, Q(123)), (0, Q(1)))
        self.assertEqual(self.call(0, 1, Q(123)), (0, Q(0)))
        self.assertEqual(self.close_and_events().count("callback_free"), 1)

    def test_ticket_uses_official_buffer_and_is_cancelled_before_user_release(self):
        self.start()
        self.initialized()
        status, data = self.call(self.user, self.method("SteamUser021", "GetAuthSessionTicket"), Q(1024))
        self.assertEqual(status, 0)
        self.assertEqual(data, Q(987) + Q(4) + Q(4) + bytes([0xa5])*4)
        status, data = self.call(0, 1, Q(123))
        self.assertEqual((status, struct.unpack("!QQQQQQQ", data)), (0, (1, 456, 163, 0, 1, 987, 1)))
        events = self.close_and_events()
        self.assertLess(events.index("ticket_cancel"), events.index("user_release"))
        self.assertEqual(events.count("ticket_cancel"), 1)
        self.assertEqual(events.count("callback_free"), 1)

    def test_invalid_handles_sizes_and_unknown_methods_do_not_call_native_methods(self):
        self.start()
        self.initialized()
        self.assertEqual(self.call(9999, self.method("SteamUser021", "BLoggedOn")), (2, b""))
        self.assertEqual(self.call(self.user, 0xdeadbeef), (1, b""))
        self.assertEqual(self.call(self.user, self.method("SteamUser021", "GetAuthSessionTicket"), Q(65537)), (2, b""))
        self.assertEqual(self.number(self.user, "BLoggedOn", version="SteamUser021"), 1)
        self.assertNotIn("ticket_create", self.close_and_events())

    def test_release_invalidates_child_object_ids(self):
        self.start()
        self.initialized()
        status, _ = self.call(self.client, self.method("SteamClient020", "BReleaseSteamPipe"), Q(123))
        self.assertEqual(status, 2)
        self.assertEqual(self.call(self.client, self.method("SteamClient020", "ReleaseUser"), Q(123) + Q(456)), (0, b""))
        self.assertEqual(self.call(self.user, self.method("SteamUser021", "BLoggedOn")), (2, b""))
        self.assertEqual(self.number(self.client, "BReleaseSteamPipe", Q(123)), 1)
        self.assertEqual(self.close_and_events().count("user_release"), 1)

    def test_cross_application_or_wrong_nonce_closes_without_touching_steam(self):
        for app, nonce in [(481, None), (480, bytes(32))]:
            with self.subTest(app=app):
                self.start()
                with self.assertRaises(EOFError):
                    self.call(0, 0, text("SteamClient020"), app=app, nonce=nonce)
                self.connection.close()
                self.connection = None
                self.child.communicate(timeout=3)
                self.assertFalse(self.trace.exists())
                self.child = None

if __name__ == "__main__":
    unittest.main()
