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
class User:public StubISteamUser{public:
 bool BLoggedOn()override{return true;}
 CSteamID GetSteamID()override{return CSteamID((uint64)76561198000000042ull);}
 HSteamUser GetHSteamUser()override{return 456;}
 HAuthTicket GetAuthSessionTicket(void* output,int capacity,uint32* count)override{if(capacity<4)return 0;memset(output,0xa5,4);*count=4;callback=163;Event("ticket_create");return 987;}
 void CancelAuthTicket(HAuthTicket h)override{if(h!=987)abort();Event("ticket_cancel");}
};
class Apps:public StubISteamApps{public:bool BIsSubscribedApp(AppId_t app)override{return app==480;}};
class Stats:public StubISteamUserStats{public:
 bool GetAchievement(const char* name,bool* out)override{if(!out)return false;*out=!strcmp(name,"fixture");return true;}
 bool RequestCurrentStats()override{callback=getenv("MOCK_PRIVATE_CALLBACK")?1040044:1101;return true;}
};
static User user;static Apps apps;static Stats stats;
class Client:public StubISteamClient{public:
 void Set_SteamAPI_CCheckCallbackRegisteredInProcess(SteamAPI_CheckCallbackRegistered_t fn)override{Event(fn&&fn(1101)==1?"callback_filter_all":"callback_filter_none");}
 HSteamPipe CreateSteamPipe()override{Event("pipe_create");return 123;}
 bool BReleaseSteamPipe(HSteamPipe p)override{if(p!=123)abort();Event("pipe_release");return true;}
 HSteamUser ConnectToGlobalUser(HSteamPipe p)override{if(p!=123)abort();Event("user_connect");return 456;}
 void ReleaseUser(HSteamPipe p,HSteamUser u)override{if(p!=123||u!=456)abort();Event("user_release");}
 ISteamUser* GetISteamUser(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();return !strcmp(version,"SteamUser021")?&user:nullptr;}
 ISteamApps* GetISteamApps(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();return !strcmp(version,"STEAMAPPS_INTERFACE_VERSION008")?&apps:nullptr;}
 ISteamUserStats* GetISteamUserStats(HSteamUser u,HSteamPipe p,const char* version)override{if(u!=456||p!=123)abort();return !strcmp(version,"STEAMUSERSTATS_INTERFACE_VERSION012")?&stats:nullptr;}
};
static Client client;
extern "C" void* CreateInterface(const char* name,int*){return !strcmp(name,"SteamClient020")?&client:nullptr;}
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

    def test_pe_framing_handles_partial_io_without_a_standalone_length_send(self):
        # Exercise the production Exchange/Transfer bodies with deterministic
        # short I/O. No timer threshold or mocked Steam authentication success.
        source = (ROOT / "entry/src/main/cpp/linux_steam/steam_bridge/pe.cpp").read_text()
        transfer = source[source.index("static bool Transfer("):source.index("static bool Configure(")]
        exchange = source[source.index("static Status Exchange("):source.index("template<class T> void WriteArgument")]
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
#include <map>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <cstdint>
namespace hmosbridge {
static std::map<std::pair<std::string,uint32_t>,void*> interfaces;
void* Wrap(const char*,uint32_t);
[[noreturn]] static void Failed(uint32_t,int){throw std::runtime_error("unsupported");}
enum {Invalid=2};
template<bool,class R,class... A> static R Invoke(uint32_t,uint32_t,A...){
 if constexpr(std::is_same_v<R,CSteamID>)return CSteamID((uint64)0x011000010000002aull);
 else if constexpr(std::is_void_v<R>)return;
 else return {};
}
template<class... A>static void RegisterCallbackCheck(A...){ }
template<class R>static R Ticket(uint32_t,uint32_t,void*,int,uint32*){return {};}
#include "pe_methods.inc"
}
int main(){
 struct Guarded {uint64 before;CSteamID id;uint64 after;};
 for(const auto* version:{"SteamUser021","STEAMAPPS_INTERFACE_VERSION008"}){
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
        binary = self.directory / "record_return"
        sdk = Path(os.environ["STEAM_BRIDGE_REFERENCE"]) / "lsteamclient/steamworks_sdk_150"
        subprocess.run(["g++", "-std=c++17", "-O1", "-w", "-I" + str(self.generated),
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

    def start(self, source_fail=False, private_callback=False):
        environment=dict(os.environ)
        if private_callback: environment["MOCK_PRIVATE_CALLBACK"]="1"
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
