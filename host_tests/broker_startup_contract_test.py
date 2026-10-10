"""Exercise production Broker v2, Wine sender, IPC receiver and phone codec.

Linux sockets/descriptors are real. NCP launch itself is mocked; no GPU/device
claims are made. The Wine source is patched in a temporary copy only.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
options = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


NATIVE_HEADER = r'''
#pragma once
#include <stdint.h>
enum Ability_NativeChildProcess_ErrCode {
    NCP_NO_ERROR=0, NCP_ERR_INVALID_PARAM=401, NCP_ERR_NOT_SUPPORTED=801,
    NCP_ERR_INTERNAL=16000050, NCP_ERR_BUSY=16010001, NCP_ERR_TIMEOUT=16010002
};
struct NativeChildProcess_Fd { char* fdName; int32_t fd; NativeChildProcess_Fd* next; };
struct NativeChildProcess_FdList { NativeChildProcess_Fd* head; };
struct NativeChildProcess_Args { char* entryParams; NativeChildProcess_FdList fdList; };
struct NativeChildProcess_Options { int isolationMode; };
#define NCP_ISOLATION_MODE_NORMAL 0
int32_t OH_Ability_StartNativeChildProcess(char*, NativeChildProcess_Args, NativeChildProcess_Options, int32_t*);
'''
PREFIX = r'''
#include "proc/spawn_codec.h"
#include "proc/wine_child_ipc.h"
#include <AbilityKit/native_child_process.h>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
using winehua::spawn::Request;
using winehua::spawn::EncodeEntry;
using winehua::spawn::DecodeEntry;
static bool phoneMode;
static int route, audioOpened, launches, nativeStatus;
static Request delivered;
static int OpenAudio() { ++audioOpened; return open("/dev/null", O_RDONLY | O_CLOEXEC); }
bool PhoneAdapter_IsPhoneMode() { return phoneMode; }
enum class ProcessRegistration { Relabel, NativeChild, ForkChild };
void AddProcess(int32_t, const std::string&, int, ProcessRegistration) {}
void Phone_MarkDirectForkChildRegistered(int32_t) {}
void MarkWineIpcChildRegistered(int32_t) {}
void WineHuaCefUtilityProbeNoteSpawn(int32_t, int32_t, const char*) {}
int32_t StartWineChildViaIpc(const NativeChildProcess_Args&, int32_t*);
Ability_NativeChildProcess_ErrCode Phone_StartViaDirectForkServer(const char*, NativeChildProcess_Args, int32_t*);

// Minimal parcel implementation; ownership after ReadFileDescriptor matches NDK.
#define OH_IPC_SUCCESS 0
#define OH_IPC_CHECK_PARAM_ERROR -1
struct OHIPCParcel {
    std::vector<int32_t> ints;
    std::vector<std::string> strings;
    std::vector<int> fds;
    mutable size_t intAt=0, stringAt=0, fdAt=0;
};
int OH_IPCParcel_ReadInt32(const OHIPCParcel* p, int32_t* out) {
    if (p->intAt == p->ints.size()) return -1;
    *out = p->ints[p->intAt++]; return 0;
}
const char* OH_IPCParcel_ReadString(const OHIPCParcel* p) {
    return p->stringAt == p->strings.size() ? nullptr : p->strings[p->stringAt++].c_str();
}
int OH_IPCParcel_ReadFileDescriptor(const OHIPCParcel* p, int32_t* fd) {
    if (p->fdAt == p->fds.size()) return -1;
    *fd = dup(p->fds[p->fdAt++]); return *fd < 0 ? -1 : 0;
}
int OH_IPCParcel_WriteInt32(OHIPCParcel* p, int32_t value) { p->ints.push_back(value); return 0; }
struct NamedFd { std::string name; int fd=-1; };
std::mutex g_mutex, g_surfaceProbeMutex;
std::condition_variable g_ready, g_surfaceProbeCondition;
bool g_received=false, g_surfaceProbeFinished=false;
std::string g_params;
std::vector<NamedFd> g_fds;
int OnSurfaceRequest(uint32_t, const OHIPCParcel*, OHIPCParcel*) { return -1; }
'''
TEST = r'''
extern "C" int ohos_broker_spawn_child(char**, int, const char*, int*);
extern "C" int ohos_broker_spawn_wineserver(int*);

static int NativeLaunch(NativeChildProcess_Args args, int32_t* pid, int kind) {
    assert(DecodeEntry(args.entryParams, delivered));
    std::vector<std::string> names;
    for (auto* node=args.fdList.head; node; node=node->next) {
        assert(fcntl(node->fd, F_GETFD) >= 0); names.emplace_back(node->fdName);
    }
    assert(winehua::spawn::MatchNames(delivered, names));
    assert(delivered.home == "/session|home\n中文");
    assert(delivered.env.back() == "WINEPREFIX=/prefix|with\nseparator");
    route=kind; ++launches; *pid=nativeStatus ? -1 : 123; return nativeStatus;
}
int32_t OH_Ability_StartNativeChildProcess(char*, NativeChildProcess_Args args, NativeChildProcess_Options, int32_t* pid) {
    return NativeLaunch(args, pid, 0);
}
int32_t StartWineChildViaIpc(const NativeChildProcess_Args& args, int32_t* pid) {
    OHIPCParcel request, reply;
    std::vector<NativeChildProcess_Fd*> nodes;
    for (auto* node=args.fdList.head; node; node=node->next) nodes.push_back(node);
    request.ints={winehua::wineipc::kVersion, static_cast<int32_t>(nodes.size())};
    request.strings.push_back(args.entryParams);
    // A named descriptor list has no ordering contract.
    for (auto it=nodes.rbegin(); it != nodes.rend(); ++it) {
        request.strings.emplace_back((*it)->fdName); request.fds.push_back((*it)->fd);
    }
    assert(OnRequest(winehua::wineipc::kBootstrap, &request, &reply, nullptr) == 0);
    assert(g_received && g_params == args.entryParams);
    std::vector<NativeChildProcess_Fd> incoming(g_fds.size());
    for (size_t i=0; i<g_fds.size(); ++i)
        incoming[i]={g_fds[i].name.data(), g_fds[i].fd, i+1<incoming.size() ? &incoming[i+1] : nullptr};
    NativeChildProcess_Args child{g_params.data(), {incoming.empty() ? nullptr : incoming.data()}};
    int result=NativeLaunch(child, pid, 1);
    CloseFds(g_fds); g_fds.clear(); g_received=false; return result;
}
Ability_NativeChildProcess_ErrCode Phone_StartViaDirectForkServer(const char* entry, NativeChildProcess_Args args, int32_t* pid) {
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
    ForkRequest outgoing, incoming;
    strcpy(outgoing.entry, entry); outgoing.params=args.entryParams;
    outgoing.paramsBytes=outgoing.params.size()+1;
    std::vector<int> sent, received;
    for (auto* node=args.fdList.head; node; node=node->next) {
        strcpy(outgoing.names[sent.size()], node->fdName); sent.push_back(node->fd);
    }
    outgoing.fdCount=sent.size();
    assert(SendForkRequest(sockets[0], outgoing, sent));
    assert(ReceiveForkRequest(sockets[1], incoming, received));
    assert(incoming.params == outgoing.params);
    std::vector<NativeChildProcess_Fd> nodes(received.size());
    for (size_t i=0; i<received.size(); ++i)
        nodes[i]={incoming.names[i], received[i], i+1<received.size() ? &nodes[i+1] : nullptr};
    NativeChildProcess_Args child{incoming.params.data(), {nodes.empty() ? nullptr : nodes.data()}};
    const int result=NativeLaunch(child, pid, 2);
    for (int fd : received) close(fd);
    close(sockets[0]); close(sockets[1]);
    return static_cast<Ability_NativeChildProcess_ErrCode>(result);
}
static int FdCount() {
    DIR* d=opendir("/proc/self/fd"); assert(d); int count=0;
    while (auto* item=readdir(d)) if (item->d_name[0] != '.') ++count;
    closedir(d); return count;
}
static void Send(int fd, const Request& r, const std::vector<int>& fds, bool raw=false) {
    auto argv=winehua::spawn::Pointers(r.argv), env=winehua::spawn::Pointers(r.env), names=winehua::spawn::Pointers(r.names);
    vp_spawn_spec s{r.home.c_str(), r.bin.c_str(), argv.data(), env.data(), names.data(),
        static_cast<uint32_t>(argv.size()), static_cast<uint32_t>(env.size()), static_cast<uint32_t>(names.size())};
    if (!raw) { assert(!vp_send_request(fd, &s, fds.data(), vp_now_ms()+3000)); return; }
    unsigned char* payload=nullptr; size_t length=0; assert(!vp_encode(&s, &payload, &length));
    std::vector<unsigned char> bytes(VP_FRAME_HEADER+length);
    memcpy(bytes.data(), "VPBR", 4); vp_store32(bytes.data()+4, 2); vp_store32(bytes.data()+8, length);
    memcpy(bytes.data()+VP_FRAME_HEADER, payload, length); free(payload);
    assert(!vp_send_bytes(fd, bytes.data(), bytes.size(), fds.data(), fds.size(), vp_now_ms()+3000));
}
static int RoundTrip(const Request& r, size_t passed, bool raw=false) {
    int baseline=FdCount(), sockets[2], status=-999, pid=-1;
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    std::vector<int> fds;
    for (size_t i=0; i<passed; ++i) { int fd=open("/dev/null", O_RDONLY); assert(fd>=0); fds.push_back(fd); }
    {
        BrokerJob job(sockets[1]);
        std::thread sender([&] { Send(sockets[0], r, fds, raw); assert(!vp_recv_reply(sockets[0], &pid, &status, vp_now_ms()+5000)); });
        if (ReceiveJob(job)) LaunchJob(job);
        sender.join();
        for (int fd : fds) assert(fcntl(fd, F_GETFD)>=0); // SCM_RIGHTS never consumes sender originals.
    }
    for (int fd : fds) close(fd);
    close(sockets[0]); assert(FdCount()==baseline); return status;
}
static Request Basic() { return {"", "/bin|dir\n中文", {"wine", "C:\\game.exe", "", "a|b\n中文", "__env=literal"}, {"AUTH=private|token\n中文"}, {}}; }
static void Codec() {
    auto r=Basic(); std::string entry; Request decoded;
    assert(EncodeEntry(r, entry) && DecodeEntry(entry.c_str(), decoded));
    assert(decoded.argv==r.argv && decoded.env==r.env && decoded.bin==r.bin);
    assert(!DecodeEntry("VPP2:===!", decoded));
    assert(!DecodeEntry("SPAWN\n", decoded));
    r.argv.push_back(std::string("embedded\0nul", 12)); assert(!EncodeEntry(r, entry));
    r=Basic(); r.env={"NO_EQUALS"}; assert(!EncodeEntry(r, entry));
    r=Basic(); r.names={std::string(20, 'n')}; assert(EncodeEntry(r, entry));
    r.names={std::string(21, 'n')}; assert(!EncodeEntry(r, entry));
    r.names={"same", "same"}; assert(!EncodeEntry(r, entry));
    r=Basic(); r.argv.clear(); assert(!EncodeEntry(r, entry));
    r=Basic(); r.argv.resize(VP_ITEMS_CAP+1, ""); assert(!EncodeEntry(r, entry));
    r=Basic(); r.argv.push_back(std::string(VP_PAYLOAD_CAP, 'x')); assert(!EncodeEntry(r, entry));
    // Independently constructed wire payload, including an empty argument.
    const unsigned char golden[]={2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,'b',1,0,0,0,'w',0,0,0,0};
    vp_spawn_spec spec{}; assert(!vp_decode(golden, sizeof(golden), &spec));
    assert(spec.argc==2 && !strcmp(spec.bin,"b") && !strcmp(spec.argv[0],"w") && !spec.argv[1][0]); vp_free_spec(&spec);
    uint32_t random=123;
    for (size_t n=0; n<1000; ++n) {
        std::vector<unsigned char> bytes(n % 512);
        for (auto& ch : bytes) { random=random*1664525u+1013904223u; ch=random>>24; }
        (void)vp_decode(bytes.data(), bytes.size(), &spec); vp_free_spec(&spec);
    }
    puts("codec: golden wire data, UTF-8, empty/separator arguments, malformed input and caps passed");
}
static void Routes() {
    for (int mode=0; mode<3; ++mode) {
        phoneMode=mode==2;
        for (size_t count : {size_t(1), size_t(63), size_t(64), size_t(256)}) {
            auto r=Basic(); r.argv.resize(count, ""); r.argv[0]="wine";
            r.env.push_back(mode==1 ? "WINEHUA_DIRECT_NCP=1" : "WINEHUA_DIRECT_NCP=0");
            if (mode==2) r.env.push_back("WINEHUA_PHONE_DIRECT_FORK=1");
            assert(!RoundTrip(r, 0)); assert(route==mode && delivered.argv==r.argv);
            assert(delivered.env[0]==r.env[0]);
        }
        for (size_t size : {size_t(15*1024), size_t(16*1024), size_t(17*1024), size_t(64*1024)}) {
            auto r=Basic(); r.argv.push_back(std::string(size,'x'));
            r.env.push_back(mode==1 ? "WINEHUA_DIRECT_NCP=1" : "WINEHUA_DIRECT_NCP=0");
            if (mode==2) r.env.push_back("WINEHUA_PHONE_DIRECT_FORK=1");
            assert(!RoundTrip(r, 0)); assert(route==mode && delivered.argv==r.argv);
        }
        auto failed=Basic();
        failed.env.push_back(mode==1 ? "WINEHUA_DIRECT_NCP=1" : "WINEHUA_DIRECT_NCP=0");
        if (mode==2) failed.env.push_back("WINEHUA_PHONE_DIRECT_FORK=1");
        nativeStatus=NCP_ERR_INTERNAL; assert(RoundTrip(failed,0)==NCP_ERR_INTERNAL); nativeStatus=0;
    }
    phoneMode=false;
    auto r=Basic();
    for (size_t i=0; i<VP_CLIENT_FDS_CAP; ++i) r.names.push_back("fd"+std::to_string(i));
    assert(!RoundTrip(r, VP_CLIENT_FDS_CAP)); assert(delivered.names.size()==VP_FDS_CAP);
    r.names.push_back("extra"); assert(RoundTrip(r, VP_FDS_CAP, true)==-EPROTO);
    r=Basic(); r.names={"one"}; assert(RoundTrip(r, 2, true)==-EPROTO);
    r.names={"wine_audio_bootstrap"}; assert(RoundTrip(r,1)==NCP_ERR_INVALID_PARAM);
    const int before=audioOpened;
    r=Basic(); r.argv={"wine", std::string(112500,'x')}; assert(RoundTrip(r,0)==-E2BIG);
    assert(audioOpened==before); // final base64+home+prefix budget is checked before launch.
    puts("launch: Start, real IPC receiver and real phone packets preserve 64 KiB/256 argv; fd and final budgets passed");
}
static void FixedImageRoute() {
    // The non-PIE harness occupies the same preferred base used by the fixture.
    char path[]="/tmp/vp-broker-fixed-image-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0);
    unsigned char image[512]{};
    image[0]='M'; image[1]='Z'; image[60]=64;
    auto p=image+64; p[0]='P'; p[1]='E'; p[4]=0x4c; p[5]=1;
    p[20]=224; p[22]=2; p[24]=0x0b; p[25]=1;
    p[54]=0x40; p[82]=1;
    assert(write(fd,image,sizeof(image))==sizeof(image)); close(fd);
    phoneMode=false; SetBrokerDirectNcpSessionDefault(true);
    auto r=Basic(); r.argv={"wine",path};
    assert(!RoundTrip(r,0)); assert(route==0);
    r.env.push_back("WINEHUA_DIRECT_NCP=1");
    assert(!RoundTrip(r,0)); assert(route==0);
    // Relocatable PE32 continues using the session's Direct creation path.
    fd=open(path,O_WRONLY); assert(fd>=0); image[158]=0x40;
    assert(write(fd,image,sizeof(image))==sizeof(image)); close(fd);
    assert(!RoundTrip(r,0)); assert(route==1);
    SetBrokerDirectNcpSessionDefault(false); unlink(path);
}
static void Readers() {
    auto queues=std::make_shared<BrokerQueues>();
    std::vector<std::thread> readers;
    for (size_t i=0; i<kReaderCount; ++i) readers.emplace_back(Reader, queues);
    std::thread dispatcher(Dispatcher, queues);
    std::vector<int> partial;
    for (int i=0; i<3; ++i) {
        int s[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s)); partial.push_back(s[0]);
        assert(send(s[0],"V",1,0)==1);
        std::lock_guard<std::mutex> lock(queues->mutex); queues->incoming.push_back(std::make_unique<BrokerJob>(s[1]));
    }
    int s[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s));
    {
        std::lock_guard<std::mutex> lock(queues->mutex); queues->incoming.push_back(std::make_unique<BrokerJob>(s[1]));
    }
    queues->readable.notify_all(); Send(s[0], Basic(), {});
    int pid=-1, status=-1; assert(!vp_recv_reply(s[0],&pid,&status,vp_now_ms()+1000)); assert(pid==123 && !status);
    close(s[0]); for (int fd : partial) close(fd);
    { std::lock_guard<std::mutex> lock(queues->mutex); queues->running=false; }
    queues->readable.notify_all(); queues->launchable.notify_all();
    for (auto& reader : readers) reader.join();
    dispatcher.join();
    // A full ready queue produces busy without calling NCP.
    queues=std::make_shared<BrokerQueues>();
    std::vector<int> held;
    for (size_t i=0; i<kQueueCapacity; ++i) {
        int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair)); held.push_back(pair[0]);
        queues->ready.push_back(std::make_unique<BrokerJob>(pair[1]));
    }
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,s));
    queues->incoming.push_back(std::make_unique<BrokerJob>(s[1]));
    std::thread reader(Reader, queues); Send(s[0],Basic(),{});
    assert(!vp_recv_reply(s[0],&pid,&status,vp_now_ms()+1000)); assert(status==NCP_ERR_BUSY);
    close(s[0]); { std::lock_guard<std::mutex> lock(queues->mutex); queues->running=false; }
    queues->readable.notify_all(); reader.join(); queues.reset(); for (int fd : held) close(fd);
    puts("broker: three stalled clients allow normal progress; bounded queue rejects busy passed");
}
static void WineClient() {
    const std::string path="/tmp/vp-broker-"+std::to_string(getpid());
    int listener=socket(AF_UNIX,SOCK_STREAM,0); assert(listener>=0);
    sockaddr_un addr{}; addr.sun_family=AF_UNIX; strcpy(addr.sun_path,path.c_str());
    unlink(path.c_str()); assert(!bind(listener,(sockaddr*)&addr,sizeof(addr)) && !listen(listener,4));
    setenv("PROCESSBROKER",path.c_str(),1); setenv("WINEBINDIR","/wine|bin\n中文",1);
    setenv("BROKER_SENTINEL","value|with\nseparator",1); setenv("WINESERVERSOCKET","do-not-forward",1);
    setenv("WINE_OHOS_AUDIO_BOOTSTRAP_FD","do-not-forward",1);
    for (int box=0; box<2; ++box) {
        if (box) setenv("USE_LIBBOX64","1",1); else unsetenv("USE_LIBBOX64");
        std::thread server([&] { int fd=accept(listener,nullptr,nullptr); assert(fd>=0); BrokerJob job(fd); assert(ReceiveJob(job)); LaunchJob(job); });
        auto r=Basic(); r.argv.resize(256, ""); r.argv[100]=std::string(64*1024,'x');
        auto args=winehua::spawn::Pointers(r.argv); args.push_back(nullptr);
        int socketFd=open("/dev/null",O_RDONLY), pid=-1;
        assert(!ohos_broker_spawn_child(const_cast<char**>(args.data()),socketFd,nullptr,&pid)); assert(pid==123);
        server.join(); close(socketFd);
        auto expected=r.argv; if (!box) expected.insert(expected.begin(),"wine");
        assert(delivered.argv==expected && delivered.bin=="/wine|bin\n中文");
        assert(std::find(delivered.env.begin(),delivered.env.end(),"BROKER_SENTINEL=value|with\nseparator")!=delivered.env.end());
        for (const auto& env : delivered.env) assert(env.rfind("WINESERVERSOCKET=",0)!=0 && env.rfind("WINE_OHOS_AUDIO_BOOTSTRAP_FD=",0)!=0);
    }
    // A BAT changes the Windows child environment, while Unix environ still
    // holds the session default. Exercise the real C sender and Broker framing.
    setenv("WINEDEBUG","-all",1);
    setenv("WINEHUA_WINEDEBUG","-all,+old",1);
    for (const char *debug : {"WINEDEBUG=-all,+win,+msg", "WINEDEBUG=-all", "WINEDEBUG="}) {
        std::thread debugServer([&] { int fd=accept(listener,nullptr,nullptr); BrokerJob job(fd); assert(ReceiveJob(job)); LaunchJob(job); });
        char *args[] = {const_cast<char*>("war3.exe"),nullptr};
        int fd=open("/dev/null",O_RDONLY), child=-1;
        assert(!ohos_broker_spawn_child(args,fd,debug,&child) && child==123);
        debugServer.join(); close(fd);
        std::vector<std::string> overrides;
        for (const auto &line : delivered.env) if (line.rfind("WINEHUA_WINEDEBUG=",0)==0) overrides.push_back(line);
        const char *expected = !strcmp(debug,"WINEDEBUG=-all") ? "WINEHUA_WINEDEBUG=-all,+old" :
            !strcmp(debug,"WINEDEBUG=") ? "WINEHUA_WINEDEBUG=-all" : "WINEHUA_WINEDEBUG=-all,+win,+msg";
        assert(overrides.size()==1 && overrides[0]==expected);
        assert(!strcmp(getenv("WINEDEBUG"),"-all") && !strcmp(getenv("WINEHUA_WINEDEBUG"),"-all,+old"));
    }
    char *invalidArgs[] = {const_cast<char*>("war3.exe"),nullptr};
    int invalidFd=open("/dev/null",O_RDONLY), invalidPid=-1;
    assert(ohos_broker_spawn_child(invalidArgs,invalidFd,"X",&invalidPid)==-1 && errno==EINVAL);
    close(invalidFd);
    unsetenv("WINEHUA_WINEDEBUG"); unsetenv("WINEDEBUG");
    std::thread server([&] { int fd=accept(listener,nullptr,nullptr); BrokerJob job(fd); assert(ReceiveJob(job)); LaunchJob(job); });
    int pid=-1; assert(!ohos_broker_spawn_wineserver(&pid)); server.join();
    assert((delivered.argv==std::vector<std::string>{"wineserver","-f","-p"}));
    std::thread appServer([&] { int fd=accept(listener,nullptr,nullptr); BrokerJob job(fd); assert(ReceiveJob(job)); LaunchJob(job); });
    auto app=Basic(); app.argv.resize(256, ""); app.argv[100]=std::string(64*1024,'a');
    app.env.insert(app.env.end(), {"WINESERVERSOCKET=stale", "WINE_OHOS_AUDIO_ENABLE=stale", "WINE_OHOS_AUDIO_BOOTSTRAP_FD=stale", "WINE_OHOS_AUDIO_PROTOCOL_VERSION=stale"});
    assert(SpawnViaBroker(app.bin,app.argv,app.env)==123); appServer.join();
    assert(delivered.argv==app.argv && delivered.env[0]==app.env[0]);
    for (const auto& env : delivered.env) assert(!vp_is_process_env(env.c_str()));
    close(listener); unlink(path.c_str());
    puts("Wine: actual patched C sender retains arguments/environ and filters only process-local handles passed");
}
int main() {
    gBrokerHomeDir="/session|home\n中文"; gBrokerPrefixDir="/prefix|with\nseparator";
    const int baseline=FdCount(); Codec(); Routes(); FixedImageRoute(); Readers(); WineClient(); assert(FdCount()==baseline);
    puts("Broker startup contract PASS (real sockets/fds; mocked OS process creation).");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='broker-contract-') as directory:
        folder = Path(directory)
        stubs = {
            'AbilityKit/native_child_process.h': NATIVE_HEADER,
            'hilog/log.h': '#pragma once\n#define LOG_APP 0\ntemplate<class... Args> void TestLog(Args...) {}\n#define OH_LOG_INFO(...) TestLog(__VA_ARGS__)\n#define OH_LOG_WARN(...) TestLog(__VA_ARGS__)\n#define OH_LOG_ERROR(...) TestLog(__VA_ARGS__)\n',
            'common/wait_utils.h': 'template<class F> bool WaitFor(const char*, F, int, int) { return true; }\n',
            'wine/wine_constants.h': '#define WINE_BROKER_SOCKET "/tmp/vp-unused-broker"\n',
            'audio/audio_broker.h': 'namespace winehua { class AudioBroker { public: static AudioBroker& GetInstance() { static AudioBroker a; return a; } int CreateBootstrapHandle() { return OpenAudio(); } }; }\n',
            'broker.h': '', 'wine_process.h': '', 'wine_child_ipc_launcher.h': '',
            'phone_adapter/phone_adapter.h': '', 'phone_adapter/phone_process.h': '', 'cef_utility_probe.h': '',
            'config.h': '#define _GNU_SOURCE 1\n',
            'wine/debug.h': '#define WINE_DEFAULT_DEBUG_CHANNEL(x)\n#define WARN(...) ((void)0)\n',
        }
        for name, content in stubs.items():
            path = folder / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        wine = folder / 'wine-copy'
        relative = 'dlls/ntdll/unix/ohos_broker.c'
        path = wine / relative
        path.parent.mkdir(parents=True)
        path.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=options.wine_src))
        public_header = 'dlls/ntdll/unix/ohos_broker.h'
        (wine / public_header).write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + public_header], cwd=options.wine_src))
        process_relative = 'dlls/ntdll/unix/process.c'
        (wine / process_relative).write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + process_relative], cwd=options.wine_src))
        subprocess.run(['patch', '-s', '-p1', '-i', str(ROOT / 'patches/wine/0020-broker-v2-startup-contract.patch')], cwd=wine, check=True)
        subprocess.run(['patch', '-s', '-p1', '-i', str(ROOT / 'patches/wine/0028-ohos-child-winedebug-environment.patch')], cwd=wine, check=True)
        assert (path.parent / 'ohos_spawn_protocol.h').read_bytes() == (ROOT / 'entry/src/main/cpp/proc/spawn_protocol.h').read_bytes()
        flags = ['-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
        obj = folder / 'wine.o'
        subprocess.run(['cc', '-std=gnu11', *flags, '-Wno-unused-variable', '-I' + str(folder), '-c', str(path), '-o', str(obj)], check=True)
        broker = (ROOT / 'entry/src/main/cpp/proc/broker.cpp').read_text()
        (folder / 'broker.cpp').write_text(broker)
        ipc = (ROOT / 'entry/src/main/cpp/proc/wine_child_ipc.cpp').read_text()
        phone = (ROOT / 'entry/src/main/cpp/phone_adapter/phone_process.cpp').read_text()
        structures = phone[phone.index('constexpr uint32_t kForkMagic'):phone.index('struct ForkReply {')]
        production = (function(ipc, 'void CloseFds(') + function(ipc, 'int OnRequest(') + structures +
                      function(phone, 'bool SendForkRequest(') + function(phone, 'bool ReceiveForkRequest('))
        harness = folder / 'test.cpp'
        client = function((ROOT / 'entry/src/main/cpp/wine/wine_exe.cpp').read_text(), 'pid_t SpawnViaBroker(')
        harness.write_text(PREFIX + production + '\n#include "broker.cpp"\n' + client + TEST)
        binary = folder / 'test'
        subprocess.run(['g++', '-std=c++17', *flags, '-pthread', '-I' + str(folder),
                        '-I' + str(ROOT / 'entry/src/main/cpp'), '-I' + str(ROOT / 'entry/src/main/cpp/proc'),
                        str(harness), str(obj), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
        transport = folder / 'transport'
        subprocess.run(['cc', '-std=gnu11', *flags, '-pthread', '-I' + str(ROOT / 'entry/src/main/cpp'),
                        str(ROOT / 'host_tests/spawn_transport_fault_test.c'), '-o', str(transport)], check=True)
        subprocess.run([str(transport)], check=True, timeout=10)


if __name__ == '__main__':
    main()
