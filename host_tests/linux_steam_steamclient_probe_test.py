#!/usr/bin/env python3
"""Exercise versioned Steam ABI failure/cleanup using a host-only fake module.

A fake result is never device evidence or Steam authentication acceptance.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import socket
import struct
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
MOCK = r"""
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int peer_socket=-1;
static void *cv[16], *uv[3], *av[7];
static void **client=cv, **user=uv, **apps=av;
static bool mode(const char *s) { const char *v=getenv("MOCK_MODE");return v&&!strcmp(v,s); }
static void event(const char *s) { FILE *f=fopen(getenv("MOCK_TRACE"),"a");if(f){fprintf(f,"%s\n",s);fclose(f);} }
static void require(bool test) { if(!test) abort(); }
static int pipe_create(void *s) {require(s==&client);event("create_pipe");return mode("pipe")?0:123;}
static bool pipe_release(void *s,int p) {require(s==&client&&p==123);event("release_pipe");return !mode("release");}
static int connect_user(void *s,int p) {
 require(s==&client&&p==123);event("connect_user");
 const char *port=getenv("MOCK_PORT");
 if(port) {
  peer_socket=socket(AF_INET,SOCK_STREAM,0);
  struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(atoi(port)),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  require(peer_socket>=0&&!connect(peer_socket,(struct sockaddr*)&target,sizeof(target)));
  char ready;require(recv(peer_socket,&ready,1,0)==1&&ready=='R');
 }
 return mode("user")?0:456;
}
static void release_user(void *s,int p,int u) {require(s==&client&&p==123&&u==456);event("release_user");if(peer_socket>=0){close(peer_socket);peer_socket=-1;}}
static void *get_user(void *s,int u,int p,const char *v) {
 require(s==&client&&u==456&&p==123&&!strcmp(v,"SteamUser021"));
 event("get_user");return mode("user_interface")?NULL:&user;
}
static void *get_apps(void *s,int u,int p,const char *v) {
 require(s==&client&&u==456&&p==123&&!strcmp(v,"STEAMAPPS_INTERFACE_VERSION008"));
 event("get_apps");return mode("apps_interface")?NULL:&apps;
}
static bool logged_on(void *s) {require(s==&user);return !mode("offline");}
static uint64_t identity(void *s) {require(s==&user);return mode("identity")?0:76561198000000042ull;}
static bool subscribed(void *s,uint32_t id) {require(s==&apps);event("ownership");return id==480;}
void *CreateInterface(const char *version,int *result) {
 require(!strcmp(version,"SteamClient020")&&!result);event("factory");
 if(mode("client"))return NULL;
 cv[0]=pipe_create;cv[1]=pipe_release;cv[2]=connect_user;cv[4]=release_user;cv[5]=get_user;cv[15]=get_apps;
 uv[1]=logged_on;uv[2]=identity;av[6]=subscribed;return &client;
}
bool Steam_BGetCallback(void) {return false;}
void Steam_FreeLastCallback(void) {}
bool Steam_GetAPICallResult(void) {return false;}
"""


class SteamClientProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="steamclient-host-test-")
        cls.directory = Path(cls.temp.name)
        cls.binary = cls.directory / "probe"
        cls.library = cls.directory / "mock.so"
        source = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/steamclient_probe.c"
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), str(source.with_name("steamclient_rpc.c")), str(source.with_name("steamclient_identity.c")), "-DSTEAMOHOS_RPC_LIFETIME_MS=2000",
                        "-ldl", "-o", str(cls.binary)], check=True)
        mock = cls.directory / "mock.c"
        mock.write_text(MOCK)
        subprocess.run(["gcc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", str(mock),
                        "-o", str(cls.library)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def probe(self, mode="", extra=(), library=None):
        trace = self.directory / "trace"
        trace.unlink(missing_ok=True)
        env = dict(os.environ, MOCK_MODE=mode, MOCK_TRACE=str(trace))
        p = subprocess.run([str(self.binary), "--library", str(library or self.library), *extra],
                           capture_output=True, text=True, timeout=3, env=env)
        events = trace.read_text().splitlines() if trace.exists() else []
        if p.returncode == 2:
            return p, None, events
        result = json.loads(p.stdout.split("STEAMOHOS_STEAMCLIENT_RESULT ", 1)[1])
        self.assertNotIn("76561198000000042", p.stdout + p.stderr)
        self.assertFalse(result["authenticationVerified"])
        self.assertFalse(result["steamApiInitVerified"])
        return p, result, events

    def test_logged_on_cleanup_and_capabilities(self):
        p, r, events = self.probe()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(r["status"], "steamclient_readonly_verified")
        self.assertTrue(r["callbackExportsAvailable"])
        self.assertTrue(r["userReleaseCalled"] and r["pipeReleaseSucceeded"])
        self.assertFalse(r["ownershipQueried"])
        self.assertEqual(events[-2:], ["release_user", "release_pipe"])

    def test_ownership_returns_real_boolean_without_treating_it_as_authentication(self):
        for value, expected in [("480", True), ("999999", False)]:
            with self.subTest(value=value):
                p, r, events = self.probe(extra=("--appid", value))
                self.assertEqual(p.returncode, 0)
                self.assertTrue(r["ownershipQueried"])
                self.assertEqual(r["subscribed"], expected)
                self.assertIn("ownership", events)

    def test_failures_release_only_acquired_objects(self):
        for mode, status, user, pipe in [
            ("client", "client_interface_unavailable", False, False),
            ("pipe", "pipe_unavailable", False, False),
            ("user", "user_unavailable", False, True),
            ("user_interface", "user_interface_unavailable", True, True),
            ("apps_interface", "apps_interface_unavailable", True, True),
            ("offline", "steam_offline", True, True),
            ("identity", "user_identity_unavailable", True, True),
            ("release", "pipe_release_failed", True, True),
        ]:
            with self.subTest(mode=mode):
                p, r, events = self.probe(mode)
                self.assertNotEqual(p.returncode, 0)
                self.assertEqual(r["status"], status)
                self.assertEqual("release_user" in events, user)
                self.assertEqual("release_pipe" in events, pipe)
                self.assertLessEqual(events.count("release_user"), 1)
                self.assertLessEqual(events.count("release_pipe"), 1)

    def test_source_identity_matches_real_live_socket_peer(self):
        self.live_peer_query(True)

    def test_source_rejects_other_process_despite_valid_login_and_library(self):
        self.live_peer_query(False)

    def live_peer_query(self, correct):
        root = self.directory / "identity-runtime"
        (root / "steamrtarm64").mkdir(parents=True, exist_ok=True)
        server = root / "steamrtarm64/steam"
        metadata = root / "metadata.so"
        source = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/socket_metadata.c"
        subprocess.run(["gcc", "-shared", "-fPIC", "-O2", str(source), "-pthread", "-o", str(metadata)], check=True)
        fixture = root / "server.c"
        fixture.write_text(r"""
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
extern void SteamOhosObserveSocket(int);
int main(void) {
 int listener=socket(AF_INET,SOCK_STREAM,0);
 struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 socklen_t size=sizeof(address);
 if(bind(listener,(struct sockaddr*)&address,size)||listen(listener,1)||getsockname(listener,(struct sockaddr*)&address,&size))return 2;
 printf("%u\n",ntohs(address.sin_port));fflush(stdout);
 int accepted=accept(listener,0,0);if(accepted<0)return 3;
 SteamOhosObserveSocket(accepted);
 if(send(accepted,"R",1,0)!=1)return 4;
 char byte;if(read(0,&byte,1)!=1)return 5;
 close(accepted);close(listener);return 0;
}
""")
        subprocess.run(["gcc", "-O2", str(fixture), str(source), "-pthread", "-o", str(server)], check=True)
        env = dict(os.environ, STEAMOHOS_SOCKET_METADATA="1", STEAMOHOS_SOCKET_METADATA_ROOT=str(root),
                   STEAMOHOS_CODE_ROOT=str(root), MOCK_TRACE=str(root / "trace"))
        child = subprocess.Popen([str(server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            port = child.stdout.readline().strip()
            self.assertTrue(port.isdigit())
            probe = subprocess.run([str(self.binary), "--library", str(self.library)], capture_output=True, text=True, timeout=3,
                env=dict(env, LD_PRELOAD=str(metadata), MOCK_PORT=port,
                         STEAMOHOS_STEAMCLIENT_EXPECTED_PID=str(child.pid if correct else os.getpid()), STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE=str(server)))
            result = json.loads(probe.stdout.split("STEAMOHOS_STEAMCLIENT_RESULT ", 1)[1])
            self.assertEqual(probe.returncode, 0 if correct else 14, probe.stderr)
            self.assertEqual(result["clientSourceVerified"], correct)
            self.assertTrue(result["loggedOn"])
            self.assertTrue(result["source"]["expectedImageNative"])
            self.assertTrue(result["source"]["loadedLibraryNative"])
            self.assertEqual(result["source"]["peerMatched"], correct)
            self.assertTrue(result["pipeReleaseSucceeded"])
            child.communicate("X", timeout=2)
            self.assertEqual(child.returncode, 0)
        finally:
            if child.poll() is None: child.kill(); child.communicate(timeout=2)

    def test_source_gate_cannot_be_satisfied_by_cached_login(self):
        trace = self.directory / "identity-trace"
        result = subprocess.run([str(self.binary), "--library", str(self.library)],
            capture_output=True, text=True, timeout=3, env=dict(os.environ,
            MOCK_TRACE=str(trace), STEAMOHOS_STEAMCLIENT_EXPECTED_PID="2147483647",
            STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE=str(self.binary)))
        record = json.loads(result.stdout.split("STEAMOHOS_STEAMCLIENT_RESULT ", 1)[1])
        self.assertEqual(result.returncode, 14)
        self.assertTrue(record["loggedOn"])
        self.assertTrue(record["clientSourceRequired"])
        self.assertFalse(record["clientSourceVerified"])
        self.assertTrue(record["pipeReleaseSucceeded"])

    def test_missing_module(self):
        p, r, events = self.probe(library=self.directory / "missing.so")
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(r["status"], "library_load_failed")
        self.assertFalse(r["libraryLoaded"])
        self.assertEqual(events, [])

    def test_missing_factory(self):
        empty = self.directory / "empty.so"
        subprocess.run(["gcc", "-shared", "-fPIC", "-x", "c", "-o", str(empty), "-"],
                       input="int unused;\n", text=True, check=True)
        p, r, events = self.probe(library=empty)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(r["status"], "factory_unavailable")
        self.assertEqual(events, [])

    def test_private_result_file_is_exclusive_and_contains_no_account_id(self):
        result_path = self.directory / "private-result.json"
        result_path.unlink(missing_ok=True)
        p, r, events = self.probe(extra=("--result-file", str(result_path)))
        self.assertEqual(p.returncode, 0)
        self.assertEqual(json.loads(result_path.read_text()), r)
        self.assertEqual(result_path.stat().st_mode & 0o777, 0o600)
        before = result_path.read_bytes()
        p, _, _ = self.probe(extra=("--result-file", str(result_path)))
        self.assertEqual(p.returncode, 13)
        self.assertEqual(result_path.read_bytes(), before)

    def test_rpc_version_authentication_framing_and_cleanup(self):
        endpoint = self.directory / "rpc.endpoint"
        trace = self.directory / "rpc.trace"
        trace.unlink(missing_ok=True)
        child = subprocess.Popen([str(self.binary), "--library", str(self.library), "--serve", str(endpoint)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                 env=dict(os.environ, MOCK_MODE="", MOCK_TRACE=str(trace)))
        try:
            deadline = time.monotonic() + 1.5
            text = []
            while time.monotonic() < deadline:
                if endpoint.exists():
                    text = endpoint.read_text().splitlines()
                    if len(text) == 3 and len(text[2]) == 64:
                        break
                time.sleep(0.005)
            self.assertEqual(len(text), 3)
            self.assertEqual(text[0], "HMOSDECK/1")
            self.assertEqual(endpoint.stat().st_mode & 0o777, 0o600)
            port, nonce = int(text[1]), bytes.fromhex(text[2])
            good = b"HDSQ" + struct.pack("!HHII", 1, 1, 45, 480) + nonce
            for request in [b"BAD!" + good[4:], good[:5] + b"\2" + good[6:],
                            good[:7] + b"\2" + good[8:], good[:-1] + bytes([good[-1] ^ 1]),
                            good[:12]]:
                with socket.create_connection(("127.0.0.1", port), timeout=1) as c:
                    c.sendall(request)
                    c.shutdown(socket.SHUT_WR)
                    self.assertEqual(c.recv(32), b"")
            with socket.create_connection(("127.0.0.1", port), timeout=1) as c:
                for i in range(0, len(good), 5):
                    c.sendall(good[i:i+5])
                data = b""
                while len(data) < 32:
                    part = c.recv(32 - len(data))
                    self.assertTrue(part)
                    data += part
            self.assertEqual(data[:4], b"HDSR")
            version, code, request_id, app, flags = struct.unpack("!HHIII", data[4:20])
            self.assertEqual((version, code, request_id, app), (1, 0, 45, 480))
            self.assertEqual(flags & 0xdff, 0xdff)
            self.assertEqual(data[20:], bytes(12))
            output, errors = child.communicate(timeout=4)
            self.assertEqual(child.returncode, 0, errors)
            self.assertFalse(endpoint.exists())
            self.assertNotIn(text[2], output + errors)
            self.assertNotIn("76561198000000042", output + errors)
            self.assertEqual(trace.read_text().splitlines().count("ownership"), 1)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=2)

    def test_bad_arguments_never_load_module(self):
        for args in [("--appid", "-1"), ("--appid", "0"), ("--appid", "4294967296"),
                     ("--appid", "1x"), ("--appid", " 480"), ("--unknown",),
                     ("--appid", "1", "--appid", "2")]:
            with self.subTest(args=args):
                p, r, events = self.probe(extra=args)
                self.assertEqual(p.returncode, 2)
                self.assertIsNone(r)
                self.assertEqual(events, [])


class SteamClientSupervisor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="steamclient-session-test-")
        cls.directory = Path(cls.temp.name)
        cls.binary = cls.directory / "supervisor"
        source = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/steamclient_session.c"
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-DSTEAMOHOS_PROBE_START_MS=5", "-DSTEAMOHOS_PROBE_RETRY_MS=5",
                        "-DSTEAMOHOS_PROBE_TIMEOUT_MS=50", "-DSTEAMOHOS_SOURCE_LIFETIME_MS=180", str(source), "-o", str(cls.binary)], check=True)
        stub = cls.directory / "stub.c"
        stub.write_text(r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv) {
 (void)argc;
 if(strstr(argv[0],"steamohos_gnu_steamclient")) {
   FILE *f=fopen(getenv("MOCK_TRACE"),"a");if(!f)return 8;
   fprintf(f,"%d\n",getpid());fclose(f);
   const char *m=getenv("MOCK_MODE");
   if(m&&!strcmp(m,"hang"))for(;;)pause();
   if(m&&!strcmp(m,"source")&&!getenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID"))return 7;
   return m&&!strcmp(m,"fail")?7:0;
 }
 usleep(250000);return 37;
}
""")
        cls.client = cls.directory / "client"
        cls.helper = cls.directory / "steamohos_gnu_steamclient"
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(stub), "-o", str(cls.client)], check=True)
        cls.helper.write_bytes(cls.client.read_bytes())
        cls.helper.chmod(0o700)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_success_is_not_repeated_and_client_exit_is_preserved(self):
        trace = self.directory / "success-trace"
        p = subprocess.run([str(self.binary), str(self.client)], capture_output=True, text=True,
                           timeout=3, env=dict(os.environ, STEAMOHOS_CODE_ROOT=str(self.directory),
                                               MOCK_TRACE=str(trace), MOCK_MODE=""))
        self.assertEqual(p.returncode, 37)
        self.assertEqual(len(trace.read_text().splitlines()), 1)
        self.assertIn("exit=0", p.stderr)

    def test_source_controls_are_fresh_and_stop_only_their_client(self):
        trace = self.directory / "source-trace"
        p = subprocess.run([str(self.binary), str(self.client)], capture_output=True, text=True,
            timeout=3, env=dict(os.environ, STEAMOHOS_CODE_ROOT=str(self.directory),
            STEAMOHOS_TMP_ROOT=str(self.directory), STEAMOHOS_STEAMCLIENT_SOURCE_TEST="1",
            MOCK_TRACE=str(trace), MOCK_MODE="source"))
        self.assertEqual(p.returncode, 0, p.stderr)
        pids = trace.read_text().splitlines()
        self.assertEqual(len(set(pids)), 3)
        self.assertIn("phase=before exit=7", p.stderr)
        self.assertIn("phase=after exit=7", p.stderr)
        self.assertIn("sourceVerified=1", p.stderr)
        self.assertIn("intentionalStop=1", p.stderr)

    def test_source_aborts_if_a_client_was_already_reachable(self):
        trace = self.directory / "source-abort-trace"
        p = subprocess.run([str(self.binary), str(self.client)], capture_output=True, text=True,
            timeout=3, env=dict(os.environ, STEAMOHOS_CODE_ROOT=str(self.directory),
            STEAMOHOS_TMP_ROOT=str(self.directory), STEAMOHOS_STEAMCLIENT_SOURCE_TEST="1",
            MOCK_TRACE=str(trace), MOCK_MODE=""))
        self.assertEqual(p.returncode, 15)
        self.assertEqual(len(trace.read_text().splitlines()), 1)

    def test_failed_and_timed_out_helpers_are_reaped(self):
        for mode in ["fail", "hang"]:
            with self.subTest(mode=mode):
                trace = self.directory / (mode + "-trace")
                p = subprocess.run([str(self.binary), str(self.client)], capture_output=True, text=True,
                                   timeout=3, env=dict(os.environ, STEAMOHOS_CODE_ROOT=str(self.directory),
                                                       MOCK_TRACE=str(trace), MOCK_MODE=mode))
                self.assertEqual(p.returncode, 37)
                pids = [int(x) for x in trace.read_text().splitlines()]
                self.assertGreater(len(pids), 1)
                self.assertLessEqual(len(pids), 6)
                for pid in pids:
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                if mode == "hang":
                    self.assertIn("timeout", p.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
