// HAP-owned entry for Steam's native Linux/FEX tool. Valve depots stay intact.
#include "native_fex.h"
#include <csignal>
#include <cstdio>
#include <poll.h>
#include <stdexcept>
#include <utility>
#include <sys/wait.h>
extern char** environ;
namespace {
volatile sig_atomic_t childPid=0;
volatile sig_atomic_t cancelled=0;
void Forward(int signal) {
    int saved=errno;cancelled=signal;
    if(childPid>0)kill(-childPid,signal);
    errno=saved;
}
int Error(const char* stage,int error=0) {
    fprintf(stderr,"STEAMOHOS_NATIVE_FEX status=failed stage=%s errno=%d\n",stage,error);
    return 126;
}
void Set(const char* key,const std::string& value) {
    if(setenv(key,value.c_str(),1))throw std::bad_alloc();
}
std::string Quote(const std::string& value) {
    static const char hex[]="0123456789abcdef";std::string out="\"";
    for(unsigned char c:value) {
        if(c=='\"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'\"';
}
bool Write(const std::string& path,const std::string& content) {
    int fd=open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return false;
    size_t done=0;int error=0;
    while(done<content.size()) {
        ssize_t n=write(fd,content.data()+done,content.size()-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){error=n<0?errno:EIO;break;}done+=n;
    }
    if(close(fd)&&!error)error=errno;
    if(error){errno=error;return false;}return true;
}
struct Config {
    std::string directory;
    ~Config() {
        if(directory.empty())return;
        // Remove only the three files owned by this launch, never recursive data.
        unlink((directory+"/Config.json").c_str());unlink((directory+"/app.json").c_str());
        unlink((directory+"/ThunksDB.json").c_str());
        rmdir(directory.c_str());
    }
};
std::string AppConfig() {
    using steamohos::fex::Env;
    // Preserve Steam's official per-app FEX performance/correctness settings.
    // Overrides follow the official tool's precedence over appinfo defaults.
    std::string result="{\"Config\":{";bool comma=false;
    const auto overrides=Env("STEAM_COMPAT_FEX_CONFIG");
    if(overrides.size()>8192)throw std::invalid_argument("FEX configuration too large");
    for(const auto& setting: {std::pair{"TSOEnabled","STEAM_FEX_TSOENABLED"},
                              std::pair{"Multiblock","STEAM_FEX_MULTIBLOCK"}}) {
        const auto value=Env(setting.second);char* end=nullptr;errno=0;
        const long number=strtol(value.c_str(),&end,10);
        if(!overrides.empty()||(!value.empty()&&end&&!*end&&!errno)) {
            const bool enabled=overrides.empty()?number!=0:
                overrides.find(std::string(setting.first)+":1")!=std::string::npos;
            if(comma)result+=',';
            comma=true;
            result+=Quote(setting.first)+":"+Quote(enabled?"1":"0");
        }
    }
    // Graphics capability is selected in Config.json from the matching depot;
    // a Steam preference may disable a thunk, but cannot enable a missing one.
    result+="},\"ThunksDB\":{";comma=false;
    if(!overrides.empty())for(const char* key:{"GL","Vulkan"}) {
        if(overrides.find(std::string("ThunksDB_")+key+":1")!=std::string::npos)continue;
        if(comma)result+=',';
        comma=true;result+=Quote(key)+":\"0\"";
    }
    return result+"}}\n";
}
int Run(int argc,char** argv) {
    using namespace steamohos::fex;
    Plan plan;if(!MakePlan(argc,argv,plan))return Error(plan.error.c_str());
#if defined(__aarch64__)
    constexpr unsigned hostMachine=183;
#elif defined(__x86_64__)
    constexpr unsigned hostMachine=62;
#else
#error Unsupported GNU host
#endif
    if(ElfMachine(plan.tool+"/usr/bin/FEX")!=hostMachine)return Error("fex_host_elf_missing");
    if(ElfMachine(plan.tool+"/usr/bin/FEXServer")!=hostMachine)return Error("fex_server_elf_missing");
    // Keep the installed official emulator and its own matching thunk ABI.
    const std::string host=plan.tool+"/usr/lib/aarch64-linux-gnu/fex-emu/HostThunks";
    const bool guest32=ElfMachine(plan.argv[1])==3;
    const std::string guest=plan.tool+(guest32?"/usr/share/fex-emu/GuestThunks_32":"/usr/share/fex-emu/GuestThunks");
    const bool gl=ElfMachine(host+"/libGL-host.so")==hostMachine&&ElfMachine(guest+"/libGL-guest.so")== (guest32?3:62);
    const bool vk=ElfMachine(host+"/libvulkan-host.so")==hostMachine&&ElfMachine(guest+"/libvulkan-guest.so")== (guest32?3:62);
    const bool alsa=ElfMachine(host+"/libasound-host.so")==hostMachine&&ElfMachine(guest+"/libasound-guest.so")== (guest32?3:62);
    const bool wayland=ElfMachine(host+"/libwayland-client-host.so")==hostMachine&&ElfMachine(guest+"/libwayland-client-guest.so")== (guest32?3:62);
    std::error_code error;fs::create_directories(plan.compat,error);
    if(error||Canonical(plan.compat)!=plan.compat)return Error("compatdata_create_failed",error.value());
    auto parent=plan.compat+"/fex-emu";fs::create_directory(parent,error);
    if(error||Canonical(parent)!=parent)return Error("fex_config_directory_failed",error.value());
    Config config;std::string pattern=parent+"/hmosdeck-XXXXXX";
    std::vector<char> writable(pattern.begin(),pattern.end());writable.push_back(0);
    char* created=mkdtemp(writable.data());if(!created)return Error("fex_config_create_failed",errno);
    config.directory=created;
    std::string document="{\"Config\":{\"RootFS\":"+Quote(plan.rootfs)+
        ",\"ThunkHostLibs\":"+Quote(host)+",\"ThunkGuestLibs\":"+Quote(guest)+
        ",\"X87ReducedPrecision\":\"1\",\"Env\":[\"LD_PRELOAD=\",\"LD_AUDIT=\","+
        Quote("LD_LIBRARY_PATH="+Env("LD_LIBRARY_PATH"))+","+Quote("STEAM_COMPAT_DATA_PATH="+Env("STEAM_COMPAT_DATA_PATH"))+" ]},\"ThunksDB\":{"+
        "\"GL\":\""+(gl?"1":"0")+"\",\"Vulkan\":\""+(vk?"1":"0")+
        "\",\"asound\":\""+(alsa?"1":"0")+"\",\"WaylandClient\":\""+(wayland?"1":"0")+"\"}}\n";
    // Host library paths never belong in the x86 guest's linker namespace.
    // RootFS paths are interpreted by FEX; per-game LD_LIBRARY_PATH remains
    // appended for Linux games shipping their own native libraries.
    std::string guestLibraries="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu";
    const auto suppliedLibraries=Env("LD_LIBRARY_PATH");
    // Drop only host search entries; preserve game-provided Linux libraries.
    std::string guestExtra;size_t start=0;
    while(start<suppliedLibraries.size()) {
        auto end=suppliedLibraries.find(':',start);
        auto part=suppliedLibraries.substr(start,end==std::string::npos?end:end-start);
        const auto code=Env("STEAMOHOS_CODE_ROOT"),graphics=Env("STEAMOHOS_NATIVE_GRAPHICS_ROOT");
        if(!part.empty()&&part!=code&&!Below(part,code)&&part!=graphics&&!Below(part,graphics))guestExtra+=":"+part;
        if(end==std::string::npos)break;
        start=end+1;
    }
    guestLibraries+=guestExtra;
    const auto old=Quote("LD_LIBRARY_PATH="+suppliedLibraries);
    document.replace(document.find(old),old.size(),Quote("LD_LIBRARY_PATH="+guestLibraries));
    std::string database;
    for(const char* relative:{"/usr/share/fex-emu/ThunksDB.json","/usr/bin/fex-emu/ThunksDB.json"}) {
        auto path=plan.tool+relative;int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        struct stat status{};
        if(!fstat(fd,&status)&&S_ISREG(status.st_mode)&&status.st_size>0&&status.st_size<=65536) {
            database.resize(status.st_size);size_t used=0;
            while(used<database.size()) {ssize_t n=read(fd,database.data()+used,database.size()-used);
                if(n<0&&errno==EINTR)continue;
                if(n<=0)break;
                used+=n;}
            if(used!=database.size())database.clear();
        }
        close(fd);if(!database.empty())break;
    }
    // Matching installed thunks need their database even in portable mode.
    // Without it, report missing graphics rather than silently enabling x86
    // software Mesa. A headless ELF can still run without any thunk modules.
    if((gl||vk||alsa||wayland)&&database.empty())return Error("matching_thunk_database_missing");
    if(!database.empty()&&!Write(config.directory+"/ThunksDB.json",database))return Error("thunk_database_write_failed",errno);
    if(!Write(config.directory+"/Config.json",document)||!Write(config.directory+"/app.json",AppConfig()))
        return Error("fex_config_write_failed",errno);
    Set("STEAMOHOS_NATIVE_FEX","1");Set("STEAMOHOS_NATIVE_FEX_TOOL",plan.tool);
    Set("STEAMOHOS_NATIVE_FEX_ROOTFS",plan.rootfs);
    const auto ipc=Canonical(Env("STEAMOHOS_IPC_ROOT"));
    if(ipc.empty())return Error("private_ipc_root_missing");
    uint32_t rootHash=2166136261u;
    for(unsigned char c:plan.rootfs+plan.compat){rootHash^=c;rootHash*=16777619u;}
    char suffix[16];snprintf(suffix,sizeof(suffix),"/f%08x",rootHash);
    const std::string fexIpc=ipc+suffix;fs::create_directory(fexIpc,error);
    if(error||Canonical(fexIpc)!=fexIpc)return Error("private_fex_ipc_create_failed",error.value());
    const std::string scope=plan.compat+"\n"+plan.rootfs+"\n";
    const std::string scopePath=fexIpc+"/scope";
    if(!Write(scopePath,scope)) {
        int fd=open(scopePath.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char stored[8192];
        ssize_t n=fd>=0?read(fd,stored,sizeof(stored)):-1;if(fd>=0)close(fd);
        if(n<0||std::string(stored,n)!=scope)return Error("private_fex_scope_conflict");
    }
    const std::string server=fexIpc+"/fex-emu/Server/"+std::to_string(getuid())+".FEXServer.Socket";
    if(server.size()>=108)return Error("private_fex_socket_path_too_long");
    // Steam's official FEX ignores APP_DATA_LOCATION in favour of this host
    // variable. Override it only for FEX and its server; Config.Env restores
    // the original Steam value in the emulated game environment.
    Set("STEAM_COMPAT_DATA_PATH",fexIpc);
    Set("STEAMOHOS_NATIVE_FEX_IPC",fexIpc);
    Set("FEX_SERVERSOCKETPATH",fexIpc+"/s");
    Set("FEX_PORTABLE","1");Set("FEX_ROOTFS",plan.rootfs);
    Set("FEX_APP_CONFIG_LOCATION",config.directory+"/");
    Set("FEX_APP_DATA_LOCATION",fexIpc+"/fex-emu/");Set("FEX_APP_CONFIG",config.directory+"/app.json");
    Set("FEX_THUNKHOSTLIBS",host);Set("FEX_THUNKGUESTLIBS",guest);Set("FEX_X87REDUCEDPRECISION","1");
    // Isolate the game from CEF's software GL workaround. The Steam parent
    // retains its original environment and all IPC/Wayland/audio capabilities.
    unsetenv("STEAMOHOS_SOFTWARE_GL");unsetenv("LIBGL_ALWAYS_SOFTWARE");
    if(!Env("VTEST_SOCKET_NAME").empty())Set("GALLIUM_DRIVER","virpipe");
    // The new game process needs a fresh bounded exec generation, not CEF's
    // accumulated handoffs. FEX helper recursion is still capped by exec_compat.
    Set("STEAMOHOS_EXEC_GENERATION","0");
    fprintf(stderr,"STEAMOHOS_NATIVE_FEX status=prepared guest_bits=%u runtime=%s gl_thunk=%d vulkan_thunk=%d audio_thunk=%d wayland_thunk=%d container=userspace\n",
        guest32?32:64,fs::path(plan.runtime).filename().c_str(),gl,vk,alsa,wayland);
    int errors[2];if(pipe2(errors,O_CLOEXEC))return Error("exec_pipe_failed",errno);
    sigset_t blocked,previous;sigemptyset(&blocked);
    for(int signal:{SIGTERM,SIGINT,SIGHUP})sigaddset(&blocked,signal);
    if(sigprocmask(SIG_BLOCK,&blocked,&previous)){close(errors[0]);close(errors[1]);return Error("signal_block_failed",errno);}
    const pid_t child=fork();
    if(child<0){int saved=errno;sigprocmask(SIG_SETMASK,&previous,nullptr);close(errors[0]);close(errors[1]);return Error("fork_failed",saved);}
    if(!child) {
        close(errors[0]);setpgid(0,0);
        for(int signal:{SIGTERM,SIGINT,SIGHUP})::signal(signal,SIG_DFL);
        sigprocmask(SIG_SETMASK,&previous,nullptr);
        std::vector<char*> args;for(auto& arg:plan.argv)args.push_back(arg.data());args.push_back(nullptr);
        execve(args[0],args.data(),environ);
        const int saved=errno;ssize_t sent;do{sent=write(errors[1],&saved,sizeof(saved));}while(sent<0&&errno==EINTR);_exit(126);
    }
    close(errors[1]);setpgid(child,child);childPid=child;
    struct sigaction action{};action.sa_handler=Forward;sigemptyset(&action.sa_mask);
    for(int signal:{SIGTERM,SIGINT,SIGHUP})sigaction(signal,&action,nullptr);
    sigprocmask(SIG_SETMASK,&previous,nullptr);
    pollfd event{errors[0],POLLIN|POLLHUP,0};int ready;
    do {ready=poll(&event,1,10000);}while(ready<0&&errno==EINTR&&!cancelled);
    int execError=0;ssize_t bytes=-1;
    if(ready>0)do {bytes=read(errors[0],&execError,sizeof(execError));}while(bytes<0&&errno==EINTR);
    close(errors[0]);
    const bool launched=ready>0&&bytes==0;
    if(!launched)kill(-child,SIGKILL);
    int status=0;pid_t waited;
    do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    childPid=0;
    if(!launched)return Error("fex_exec_failed",bytes==sizeof(execError)?execError:ETIMEDOUT);
    if(waited!=child)return Error("fex_wait_failed",errno);
    const int result=WIFEXITED(status)?WEXITSTATUS(status):WIFSIGNALED(status)?128+WTERMSIG(status):125;
    fprintf(stderr,"STEAMOHOS_NATIVE_FEX status=exited code=%d signal=%d\n",result,WIFSIGNALED(status)?WTERMSIG(status):0);
    return result;
}
}
int main(int argc,char** argv) {
    try {return Run(argc,argv);}catch(const std::bad_alloc&){return Error("out_of_memory",ENOMEM);}
    catch(const std::exception&){return Error("launch_exception",EINVAL);}
}
