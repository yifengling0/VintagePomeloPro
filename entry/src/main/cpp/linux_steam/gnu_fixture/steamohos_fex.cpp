// HAP-owned entry for Steam's native Linux/FEX tool. Valve depots stay intact.
#include "native_fex.h"
#include "native_fex_core.h"
#include "native_jit_capability.h"
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
bool DebuggerRequested() {
    if(steamohos::fex::Env("STEAMOHOS_NATIVE_MEMORY")!="1")return false;
    const auto ipc=steamohos::fex::Env("STEAMOHOS_IPC_ROOT");if(ipc.empty()||ipc[0]!='/')return false;
    int fd=open((ipc+"/native-fex-diagnostic").c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return false;
    struct stat st{};char option[16]{};bool accepted=false;
    if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_size==4)
        accepted=read(fd,option,sizeof(option))==4&&!memcmp(option,"gdb\n",4);
    close(fd);return accepted;
}
struct Config {
    std::string directory;
    ~Config() {
        if(directory.empty())return;
        // Remove only the three files owned by this launch, never recursive data.
        unlink((directory+"/Config.json").c_str());unlink((directory+"/app.json").c_str());
        unlink((directory+"/ThunksDB.json").c_str());
        rmdir((directory+"/AppConfig").c_str()); // only if FEX left it empty
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
    Plan plan;if(!MakePlan(argc,argv,plan)) {
        // Only bounded launch paths: never dump Steam's credentials/environment.
        for(const char* key:{"STEAMOHOS_LIBRARY_ROOT","STEAMOHOS_STEAM_ROOT",
                            "STEAM_COMPAT_DATA_PATH","STEAM_COMPAT_INSTALL_PATH"}) {
            const auto value=Env(key).substr(0,4096);
            fprintf(stderr,"STEAMOHOS_NATIVE_FEX_PATH key=%s requested=%s canonical=%s\n",
                key,Quote(value).c_str(),Quote(Canonical(value)).c_str());
        }
        return Error(plan.error.c_str());
    }
#if defined(__aarch64__)
    constexpr unsigned hostMachine=183;
#elif defined(__x86_64__)
    constexpr unsigned hostMachine=62;
#else
#error Unsupported GNU host
#endif
    if(ElfMachine(plan.tool+"/usr/bin/FEX")!=hostMachine)return Error("fex_host_elf_missing");
    if(ElfMachine(plan.tool+"/usr/bin/FEXServer")!=hostMachine)return Error("fex_server_elf_missing");
    const char* coreReason=nullptr;
    const bool repairedCore=SelectCore(plan,hostMachine,&coreReason);
    fprintf(stderr,"STEAMOHOS_NATIVE_FEX core=%s reason=%s patch=%s\n",repairedCore?"hap-matched":"depot",coreReason,repairedCore?"sigreturn-5901+ohos-guest-vma":"none");
    const auto jitPolicy=ApplyNativeJitCapability();
    if(jitPolicy==NativeJitPolicy::EnvironmentError)return Error("jit_capability_environment",errno);
    if(jitPolicy==NativeJitPolicy::MetadataDenied)
        fprintf(stderr,"STEAMOHOS_NATIVE_JIT runtime=coreclr wx=0 reason=memfd_metadata_denied scope=native-child\n");
    // Keep the installed official package's matching thunk ABI.
    const std::string host=plan.tool+"/usr/lib/aarch64-linux-gnu/fex-emu/HostThunks";
    const bool guest32=ElfMachine(plan.argv[1])==3;
    // FEX appends _32 itself when selecting the guest overlay directory.
    // Probe the architecture-specific files but configure the common base.
    const std::string guestBase=plan.tool+"/usr/share/fex-emu/GuestThunks";
    const std::string guest=guestBase+(guest32?"_32":"");
    const bool gl=ElfMachine(host+"/libGL-host.so")==hostMachine&&ElfMachine(guest+"/libGL-guest.so")== (guest32?3:62);
    const bool vk=ElfMachine(host+"/libvulkan-host.so")==hostMachine&&ElfMachine(guest+"/libvulkan-guest.so")== (guest32?3:62);
    // ALSA device-name strings transfer malloc ownership to the caller.
    // FEX's generated thunk returns host allocations to guest free(). Keep
    // ALSA and the existing shared-ring PCM plugin together in the guest ABI.
    const auto guestAudio=Env("STEAMOHOS_CODE_ROOT")+"/native-guest-core-x86_64";
    const bool guestAlsa=!guest32&&fs::is_directory(guestAudio);
    if(guestAlsa&&ElfMachine(guestAudio+"/libasound_module_pcm_hmosdeck.so")!=62)return Error("guest_audio_plugin_missing");
    const bool alsa=!guestAlsa&&ElfMachine(host+"/libasound-host.so")==hostMachine&&ElfMachine(guest+"/libasound-guest.so")== (guest32?3:62);
    if(guestAlsa)Set("ALSA_PLUGIN_DIR",guestAudio);
    const bool wayland=ElfMachine(host+"/libwayland-client-host.so")==hostMachine&&ElfMachine(guest+"/libwayland-client-guest.so")== (guest32?3:62);
    std::error_code error;fs::create_directories(plan.compat,error);
    if(error||Canonical(plan.compat)!=plan.compat)return Error("compatdata_create_failed",error.value());
    auto parent=plan.compat+"/fex-emu";fs::create_directory(parent,error);
    if(error||Canonical(parent)!=parent)return Error("fex_config_directory_failed",error.value());
    Config config;std::string pattern=parent+"/hmosdeck-XXXXXX";
    std::vector<char> writable(pattern.begin(),pattern.end());writable.push_back(0);
    char* created=mkdtemp(writable.data());if(!created)return Error("fex_config_create_failed",errno);
    config.directory=created;
    // Steam's ARM host PATH contains OHOS tools only. Guest subprocesses
    // (for example a JVM exec of xrandr) need the selected rootfs utilities.
    // Preserve game-supplied search order, remove known host-only entries and
    // append missing GNU defaults. This wrapper is a child; parent Steam and
    // the independent OHOS Wine launch path keep their original environment.
    std::vector<std::string> guestBins;
    const auto hostCode=Env("STEAMOHOS_CODE_ROOT");
    const auto suppliedPath=Env("PATH");size_t pathStart=0;
    while(pathStart<suppliedPath.size()) {
        const auto end=suppliedPath.find(':',pathStart);
        const auto part=suppliedPath.substr(pathStart,end==std::string::npos?end:end-pathStart);
        if(part!="/system/bin"&&part!=hostCode&&!Below(part,hostCode)&&
           std::find(guestBins.begin(),guestBins.end(),part)==guestBins.end())guestBins.push_back(part);
        if(end==std::string::npos)break;
        pathStart=end+1;
    }
    for(const char* part:{"/usr/local/bin","/usr/bin","/bin"})
        if(std::find(guestBins.begin(),guestBins.end(),part)==guestBins.end())guestBins.push_back(part);
    std::string guestPath;
    for(size_t i=0;i<guestBins.size();++i) {if(i)guestPath+=':';guestPath+=guestBins[i];}
    Set("PATH",guestPath); // first envp entry, not only FEX's appended Config.Env
    std::string document="{\"Config\":{\"RootFS\":"+Quote(plan.rootfs)+
        ",\"ThunkHostLibs\":"+Quote(host)+",\"ThunkGuestLibs\":"+Quote(guestBase)+
        // FEX's tiny-json reader consumes each Config value as a scalar.
        // StringArrayType uses repeated string properties (as emitted by
        // FEX SaveLayerToJSON), NOT a JSON array. An array silently loses the
        // guest preload and lets Steamworks hit OHOS-denied SysV syscalls.
        ",\"X87ReducedPrecision\":\"1\",\"Env\":\"LD_PRELOAD=\",\"Env\":\"LD_AUDIT=\",\"Env\":"+
        Quote("LD_LIBRARY_PATH="+Env("LD_LIBRARY_PATH"))+",\"Env\":"+Quote("PATH="+guestPath)+",\"Env\":"+Quote("STEAM_COMPAT_DATA_PATH="+Env("STEAM_COMPAT_DATA_PATH"))+"},\"ThunksDB\":{"+
        "\"GL\":\""+(gl?"1":"0")+"\",\"Vulkan\":\""+(vk?"1":"0")+
        "\",\"asound\":\""+(alsa?"1":"0")+"\",\"WaylandClient\":\""+(wayland?"1":"0")+"\"}}\n";
    // Anonymous ELF loading is a runtime capability, not a request for FEX
    // verbose logging. Keep the official quiet default; explicit FEX logging
    // preferences can still be supplied through its environment options.
    if(DebuggerRequested())document.insert(document.find("\"RootFS\""),"\"GdbServer\":\"1\",");
    // FEX applies Config.Env only to the guest loader. Keep the host's preload
    // empty below; an x86 adapter must never be loaded into the ARM64 FEX ELF.
    const auto guestIpc=guestAudio+"/libsteamohos_native_ipc.so";
    if(!guest32&&fs::is_directory(guestAudio)) {
        if(ElfMachine(guestIpc)!=62)return Error("guest_ipc_adapter_missing");
        const auto preload=Quote("LD_PRELOAD=");
        document.replace(document.find(preload),preload.size(),Quote("LD_PRELOAD="+guestIpc));
    }
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
    // Official guest thunks require a newer GNU ABI than soldier/sniper.
    // Keep the original guest ELF/entry/auxv. Only its PT_INTERP file lookup
    // selects the paired loader; core dependencies take precedence by soname.
    const auto guestCore=Env("STEAMOHOS_CODE_ROOT")+"/native-guest-core-x86_64";
    if(!guest32&&fs::is_directory(guestCore)) {
        for(const char* name:{"ld-linux-x86-64.so.2","libc.so.6","libm.so.6","libdl.so.2",
                              "libpthread.so.0","librt.so.1","libutil.so.1","libresolv.so.2",
                              "libanl.so.1","libnss_dns.so.2","libnss_files.so.2","libgcc_s.so.1","libstdc++.so.6"})
            if(ElfMachine(guestCore+"/"+name)!=62)return Error("guest_core_elf_invalid");
        if(access((guestCore+"/manifest.json").c_str(),R_OK))return Error("guest_core_not_ready");
        Set("STEAMOHOS_NATIVE_GUEST_CORE",guestCore);
        guestLibraries=guestCore+":"+guestLibraries;
    } else unsetenv("STEAMOHOS_NATIVE_GUEST_CORE");
    guestLibraries+=guestExtra;
    // This must also be the FIRST entry seen by FEX's main envp. Config.Env
    // appends entries and cannot replace an inherited ARM64 search path.
    // exec_compat supplies the independent ARM64 path to rtld on its command line.
    Set("LD_LIBRARY_PATH",guestLibraries);
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
    // The rtld normally permits external images only in the selected release.
    // Valve's matching FEX executable/thunks live in the sibling library.
    // Scope this child's anonymous ELF mapping to that SAME app installation,
    // never the shared Download directory or an unrelated library tree.
    if(Env("STEAMOHOS_NATIVE_MEMORY")=="1") {
        // realpath(code) deliberately aliases Steam's writable client data.
        // Use the inherited release spelling for the rtld capability instead.
        const auto published=Env("STEAMOHOS_EXTERNAL_RUNTIME");
        if(published.empty()||fs::path(published).lexically_normal().string()!=published||
           published!=Env("STEAMOHOS_CODE_ROOT"))return Error("fex_external_runtime_identity");
        if(!Below(plan.tool,published)) {
            std::string installation;
            for(const auto& library:Libraries()) {
                const auto parent=fs::path(library).parent_path().string();
                if(fs::path(library).filename()=="library" &&
                   Below(published,parent+"/releases") && Below(plan.tool,library+"/steamapps/common"))installation=parent;
            }
            if(installation.empty()) {
                fprintf(stderr,"STEAMOHOS_NATIVE_FEX_SCOPE runtime=%s code=%s\n",
                    Quote(published.substr(0,4096)).c_str(),
                    Quote(Env("STEAMOHOS_CODE_ROOT").substr(0,4096)).c_str());
                return Error("fex_external_runtime_scope");
            }
            Set("STEAMOHOS_EXTERNAL_RUNTIME",installation);
        }
    }
    const auto ipc=Canonical(Env("STEAMOHOS_IPC_ROOT"));
    if(ipc.empty())return Error("private_ipc_root_missing");
    uint32_t rootHash=2166136261u;
    for(unsigned char c:plan.rootfs+plan.compat){rootHash^=c;rootHash*=16777619u;}
    char suffix[16];snprintf(suffix,sizeof(suffix),"/f%08x",rootHash);
    std::string fexIpc=ipc+suffix;
    // Official Steam FEX appends a double slash and the decimal UID. Account
    // for its exact format, including a maximal UID, before starting a server.
    constexpr size_t serverSuffix=sizeof("/fex-emu/Server//4294967295.FEXServer.Socket")-1;
    if(fexIpc.size()+serverSuffix>=108) {
        const auto parent=fs::path(ipc).parent_path().string();
        if(parent!="/data/storage/el2/base/files/steamohos-native")return Error("private_fex_socket_path_too_long");
        fexIpc=parent+suffix;
    }
    fs::create_directory(fexIpc,error);
    if(error||Canonical(fexIpc)!=fexIpc)return Error("private_fex_ipc_create_failed",error.value());
    const std::string scope=plan.compat+"\n"+plan.rootfs+"\n";
    const std::string scopePath=fexIpc+"/scope";
    if(!Write(scopePath,scope)) {
        int fd=open(scopePath.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char stored[8192];
        ssize_t n=fd>=0?read(fd,stored,sizeof(stored)):-1;if(fd>=0)close(fd);
        if(n<0||std::string(stored,n)!=scope)return Error("private_fex_scope_conflict");
    }
    if(fexIpc.size()+serverSuffix>=108)return Error("private_fex_socket_path_too_long");
    // Steam's official FEX ignores APP_DATA_LOCATION in favour of this host
    // variable. Override it only for FEX and its server; Config.Env restores
    // the original Steam value in the emulated game environment.
    Set("STEAM_COMPAT_DATA_PATH",fexIpc);
    Set("STEAMOHOS_NATIVE_FEX_IPC",fexIpc);
    Set("FEX_SERVERSOCKETPATH",fexIpc+"/s");
    Set("FEX_PORTABLE","1");Set("FEX_ROOTFS",plan.rootfs);
    Set("FEX_APP_CONFIG_LOCATION",config.directory+"/");
    Set("FEX_APP_DATA_LOCATION",fexIpc+"/fex-emu/");Set("FEX_APP_CONFIG",config.directory+"/app.json");
    Set("FEX_THUNKHOSTLIBS",host);Set("FEX_THUNKGUESTLIBS",guestBase);Set("FEX_X87REDUCEDPRECISION","1");
    // Native guest socket syscalls can bypass libc interposition. Use the
    // host-created loopback display, while preserving the parent's DISPLAY.
    const auto nativeDisplay = Env("STEAMOHOS_NATIVE_X11_DISPLAY");
    if (!nativeDisplay.empty()) Set("DISPLAY", nativeDisplay);
    // Native game's implemented WSI is X11 -> Wayland -> NativeBuffer. Do not
    // let SDL auto-select the separate, incomplete direct Wayland game path.
    // Respect an explicit application/user backend choice.
    if(Env("SDL_VIDEODRIVER").empty())Set("SDL_VIDEODRIVER","x11");
    if(Env("SDL_VIDEO_DRIVER").empty())Set("SDL_VIDEO_DRIVER","x11");
    // Isolate the game from CEF's software GL workaround. The Steam parent
    // retains its original environment and all IPC/Wayland/audio capabilities.
    unsetenv("STEAMOHOS_SOFTWARE_GL");unsetenv("LIBGL_ALWAYS_SOFTWARE");
    if(!Env("VTEST_SOCKET_NAME").empty()) {
        Set("GALLIUM_DRIVER","virpipe");
        const auto glxRoot=Env("STEAMOHOS_CODE_ROOT")+"/native-vulkan-driver";
        if(fs::is_regular_file(glxRoot+"/libGL.so.1")) {
            Set("LIBGL_DRIVERS_PATH",glxRoot);
            Set("STEAMOHOS_NATIVE_GLX_ROOT",glxRoot);
        }
        // Same Venus transport and ordering contract as GNU Wine's DXVK
        // path. The unconfigured Linux ICD otherwise probes /dev/dri, which
        // is not the OHOS host's NativeBuffer rendering endpoint.
        Set("VN_DEBUG","vtest");
        Set("VN_PERF","no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring");
        for(const char* name:{"VN_WINEHUA_STRONG_RING_BARRIER","VN_WINEHUA_REMOTE_MEMORY_SYNC",
                             "VN_WINEHUA_PERSISTENT_MAP_SYNC","VN_WINEHUA_DIRECT_FENCE_WAIT","VN_OHOS_NATIVE_MEMORY"})Set(name,"1");
    }
    // The new game process needs a fresh bounded exec generation, not CEF's
    // accumulated handoffs. FEX helper recursion is still capped by exec_compat.
    Set("STEAMOHOS_EXEC_GENERATION","0");
    fprintf(stderr,"STEAMOHOS_NATIVE_FEX status=prepared guest_bits=%u runtime=%s gl_thunk=%d vulkan_thunk=%d audio_thunk=%d wayland_thunk=%d guest_alsa=%d container=userspace\n",
        guest32?32:64,fs::path(plan.runtime).filename().c_str(),gl,vk,alsa,wayland,guestAlsa);
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
