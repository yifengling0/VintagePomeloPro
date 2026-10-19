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
 bool RequestCurrentStats()override{callback=1101;return true;}
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

    def start(self, source_fail=False):
        self.child = subprocess.Popen([str(self.server), "--endpoint", str(self.endpoint)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, STEAMOHOS_CODE_ROOT=str(self.root), SteamAppId="480", STEAMOHOS_STEAMCLIENT_EXPECTED_PID=str(os.getpid()),
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
        self.assertEqual(struct.unpack("!QQQQQQQ", data), (1, 456, 1101, 0, 480, 1, 76561198000000042))
        self.assertEqual(self.call(0, 2, Q(123)), (0, Q(1)))
        self.assertEqual(self.call(0, 1, Q(123)), (0, Q(0)))
        self.assertIn("callback_free", self.close_and_events())

    def test_ticket_uses_official_buffer_and_is_cancelled_before_user_release(self):
        self.start()
        self.initialized()
        status, data = self.call(self.user, self.method("SteamUser021", "GetAuthSessionTicket"), Q(1024))
        self.assertEqual(status, 0)
        self.assertEqual(data, Q(987) + Q(4) + Q(4) + bytes([0xa5])*4)
        status, data = self.call(0, 1, Q(123))
        self.assertEqual((status, struct.unpack("!QQQQQQ", data)), (0, (1, 456, 163, 0, 987, 1)))
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
