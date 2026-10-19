// Native GNU exec handoff for installed or verified external research images. No CPU translation.
#include "../native_loader.h"
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <spawn.h>
#include <atomic>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

extern char** environ;
namespace {
using KernelExec = int (*)(const char*, char* const[], char* const[]);
KernelExec NextExec = nullptr;
using KernelSigaction = int (*)(int, const struct sigaction*, struct sigaction*);
KernelSigaction NextSigaction = nullptr;
// This handler remains valid after a new GNU TLS context takes ownership.
__attribute__((no_stack_protector)) long RawCall(long number, long a, long b, long c) {
#if defined(__aarch64__)
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return result;
#endif
}
__attribute__((no_stack_protector)) void ExecSigSys(int, siginfo_t* info, void*) {
    static const char prefix[] = "STEAMOHOS_SIGSYS syscall=";
    RawCall(SYS_write, 2, reinterpret_cast<long>(prefix), sizeof(prefix)-1);
    char number[24]; int used = 0;
    unsigned value = info ? static_cast<unsigned>(info->si_syscall) : 0;
    do { number[used++] = static_cast<char>('0' + value % 10); value /= 10; } while (value && used < 22);
    for (int i = 0; i < used / 2; ++i) { char t = number[i]; number[i] = number[used-i-1]; number[used-i-1] = t; }
    number[used++] = '\n';
    RawCall(SYS_write, 2, reinterpret_cast<long>(number), used);
    RawCall(SYS_exit_group, 159, 0, 0);
    for (;;) {}
}

__attribute__((constructor)) void InitializeExec() {
    NextExec = reinterpret_cast<KernelExec>(dlsym(RTLD_NEXT, "execve"));
    NextSigaction = reinterpret_cast<KernelSigaction>(dlsym(RTLD_NEXT, "sigaction"));
    const char* native=getenv("STEAMOHOS_NATIVE_MEMORY");
    if (NextSigaction && native && !strcmp(native,"1")) {
        struct sigaction diagnostic{};diagnostic.sa_flags=SA_SIGINFO;
        diagnostic.sa_sigaction=ExecSigSys;sigemptyset(&diagnostic.sa_mask);
        int status=NextSigaction(SIGSYS,&diagnostic,nullptr);
        fprintf(stderr,"STEAMOHOS_SIGSYS_HANDLER installed=%d\n",status==0);
    }
}
bool Under(const std::string& path, const std::string& root) {
    return !root.empty() && path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
        path[root.size()] == '/' && path.find("/../") == std::string::npos &&
        path.compare(path.size() >= 3 ? path.size()-3 : 0, 3, "/..") != 0;
}
std::string Executable(const char* filename, const std::string& code) {
    if (!filename || !*filename || code.empty()) return {};
    std::string path(filename);
    if (path == "/proc/self/exe") {
        const char* entry = getenv("STEAMOHOS_EXEC_PATH");
        if (entry) path = entry;
    }
    if(path=="/bin/sh")path=code+"/steamohos_shell";
    if(path=="/bin/lsof"||path=="/usr/bin/lsof")path=code+"/steamohos_lsof";
    if(path=="/system/bin/taskset"||path=="/usr/bin/taskset"||path=="/bin/taskset")path=code+"/steamohos_taskset";
    if(path[0]!='/') {
        char cwd[4096];if(!getcwd(cwd,sizeof(cwd)))return {};
        path=std::string(cwd)+'/'+path;
    }
    path=std::filesystem::path(path).lexically_normal().string();
    const char* home=getenv("HOME"),*data=getenv("STEAMOHOS_STEAM_ROOT");
    // Steam SDK launch glue is supplied by our tool, never by a writable client script.
    if(home && path==std::string(home)+"/.steam/sdkarm64/steam-launch-wrapper")
        path=code+"/compatibilitytools.d/hmosdeck/hmosdeck";
    if(home&&data) {
        const std::string alias=std::string(home)+"/.steam/root";
        if(Under(path,alias))path=std::string(data)+path.substr(alias.size());
    }
    if (data && Under(path, data)) path = code + path.substr(strlen(data));
    if (!Under(path, code)) return {};
    return path;
}
unsigned Threads() {
    DIR* directory = opendir("/proc/self/task");
    if (!directory) return 0;
    unsigned count = 0;
    while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
void Set(std::vector<std::string>& environment, const std::string& name, const std::string& value) {
    const std::string prefix = name + '=';
    environment.erase(std::remove_if(environment.begin(), environment.end(), [&](const std::string& entry) {
        return entry.compare(0, prefix.size(), prefix) == 0;
    }), environment.end());
    environment.push_back(prefix + value);
}
int Fail(int error) { errno = error; return -1; }
int Handoff(const char* filename, char* const arguments[], char* const supplied[]) {
    const char* configured = getenv("STEAMOHOS_CODE_ROOT");
    const std::string code = configured ? configured : "";
    const std::string target = Executable(filename, code);
    const char* diagnostic=getenv("STEAMOHOS_NATIVE_MEMORY");
    static std::atomic<unsigned> attempts{0};
    if (diagnostic && !strcmp(diagnostic,"1") && attempts.fetch_add(1)<80)
        fprintf(stderr,"STEAMOHOS_EXEC_ATTEMPT path=%s target=%s threads=%u\n",
            filename?filename:"<null>",target.empty()?"<kernel>":target.c_str(),Threads());
    if(filename&&!strcmp(filename,"/bin/sh")&&arguments)
        for(size_t i=1;i<4&&arguments[i];++i)fprintf(stderr,"STEAMOHOS_SHELL_ARG index=%zu value=%.700s\n",i,arguments[i]);
    if(diagnostic&&!strcmp(diagnostic,"1")&&arguments&&target.size()>=15&&
       target.compare(target.size()-15,15,"/steamwebhelper")==0) {
        for(size_t i=1;i<120&&arguments[i];++i)
            if(!strncmp(arguments[i],"--type=",7)||!strncmp(arguments[i],"--disable-",10)||
               !strncmp(arguments[i],"--no-",5)||!strncmp(arguments[i],"--use-",6)||
               !strncmp(arguments[i],"-disable-",9)||!strcmp(arguments[i],"-no-sandbox"))
                fprintf(stderr,"STEAMOHOS_CEF_SWITCH %.240s\n",arguments[i]);
    }
    if (code.empty() || target.empty()) {
        if (!NextExec) return Fail(ENOSYS);
        // A host exec must not feed GNU libraries to the OHOS dynamic linker.
        std::vector<char*> clean;
        if (supplied) for (size_t n=0; supplied[n] && n<256; ++n)
            if (strncmp(supplied[n], "LD_PRELOAD=", 11) && strncmp(supplied[n], "LD_LIBRARY_PATH=", 16) &&
                strncmp(supplied[n], "LD_AUDIT=", 9)) clean.push_back(supplied[n]);
        clean.push_back(nullptr);
        return NextExec(filename, arguments, clean.data());
    }
    if (Threads() != 1) return Fail(ENOTSUP);
    const char* previous = getenv("STEAMOHOS_EXEC_GENERATION");
    const unsigned generation = previous ? strtoul(previous, nullptr, 10) : 0;
    if (generation >= 8) return Fail(ELOOP);
    // The actual original ELF must exist; never treat arbitrary scripts as an ELF.
    int input = open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return -1;
    unsigned char header[20]; ssize_t bytes = read(input, header, sizeof(header)); close(input);
#if defined(__aarch64__)
    constexpr unsigned machine = 183;
    const std::string loader = code + "/ld-linux-aarch64.so.1";
#elif defined(__x86_64__)
    constexpr unsigned machine = 62;
    const std::string loader = code + "/ld-linux-x86-64.so.2";
#else
#error Unsupported GNU native ISA
#endif
    if (bytes != sizeof(header) || memcmp(header, "\177ELF", 4) || header[4] != 2 || header[5] != 1 ||
        (header[18] | (header[19] << 8)) != machine) return Fail(ENOEXEC);
    std::vector<std::string> argv{loader, "--inhibit-cache", "--library-path",
        code + ':' + code + "/steamrtarm64:" + code + "/steamrtarm64/libs:" + code + "/steamrtarm64/video",
        "--argv0", arguments && arguments[0] ? arguments[0] : target, target};
    if (arguments) for (size_t n=1; arguments[n]; ++n) {
        if (n >= 120 || strnlen(arguments[n], 16384) >= 16384) return Fail(E2BIG);
        argv.emplace_back(arguments[n]);
    }
    std::vector<std::string> environment;
    if (supplied) for (size_t n=0; supplied[n]; ++n) {
        if (n >= 220 || strnlen(supplied[n], 16384) >= 16384) return Fail(E2BIG);
        environment.emplace_back(supplied[n]);
    }
    for (const char* key : {"STEAMOHOS_CODE_ROOT", "STEAMOHOS_STEAM_ROOT", "STEAMOHOS_TMP_ROOT",
            "STEAMOHOS_LAUNCH_SOCKET", "STEAMOHOS_LAUNCH_NONCE", "STEAMOHOS_Z_ROOT",
            "STEAMOHOS_WINE_PREFIX", "STEAMOHOS_LIBRARY_ROOT",
            "STEAMOHOS_AUDIO_BOOTSTRAP_FD", "STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET", "ALSA_CONFIG_PATH", "ALSA_PLUGIN_DIR",
            "SDL_AUDIO_DRIVER", "STEAMOHOS_GAMEPAD_SOCKET",
            "STEAMOHOS_IPC_ROOT", "STEAMOHOS_SOCKET_METADATA_ROOT", "STEAMOHOS_X11_SOCKET", "XLOCALEDIR", "GLIBC_TUNABLES", "SSL_CERT_FILE",
            "STEAMOHOS_NATIVE_MEMORY", "STEAMOHOS_EXTERNAL_RUNTIME", "STEAMOHOS_SOFTWARE_GL", "GALLIUM_DRIVER", "LIBGL_ALWAYS_SOFTWARE", "VTEST_SOCKET_NAME", "XLIB_NO_SHM", "STEAMOHOS_SOCKET_METADATA"}) {
        const char* value = getenv(key); if (value) Set(environment, key, value);
    }
    const char* software=getenv("STEAMOHOS_SOFTWARE_GL");
    const char* gamepad=getenv("STEAMOHOS_GAMEPAD_SOCKET");
    Set(environment,"LD_PRELOAD",code+"/libsteamohos_posix.so"+
        (software && !strcmp(software,"1") ? ":"+code+"/libsteamohos_software_gl.so" : "")+
        (gamepad && *gamepad ? ":"+code+"/libsteamohos_gamepad.so" : ""));
    Set(environment, "STEAMOHOS_EXEC_PATH", target);
    Set(environment, "STEAMOHOS_EXEC_GENERATION", std::to_string(generation + 1));
    steamohos::NativeLoaderImage image;
    if (!image.Prepare(argv, environment)) {
        fprintf(stderr, "STEAMOHOS_GNU_EXEC_V1 prepare_failed stage=%s errno=%d\n", image.stage.c_str(), image.error);
        return Fail(image.error);
    }
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    std::vector<int> closeOnExec;
    while (auto* entry = readdir(directory)) {
        char* end = nullptr; long fd = strtol(entry->d_name, &end, 10);
        if (!end || *end || fd == dirfd(directory)) continue;
        int flags = fcntl(static_cast<int>(fd), F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC)) closeOnExec.push_back(fd);
    }
    closedir(directory);
    // A userspace handoff is permitted only after fork / in a single-thread
    // process. Old mappings remain, but no old instruction/TLS context resumes.
    struct sigaction action{}; action.sa_handler = SIG_DFL; sigemptyset(&action.sa_mask);
    for (int signal=1; signal<NSIG; ++signal) {
        if (signal == SIGKILL || signal == SIGSTOP) continue;
        struct sigaction old{};
        if (!sigaction(signal, nullptr, &old) && old.sa_handler != SIG_IGN)
            if (sigaction(signal, &action, nullptr) && errno != EINVAL) return -1;
    }
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = ExecSigSys;
    if (sigaction(SIGSYS, &action, nullptr)) return -1;
    stack_t alternate{}; alternate.ss_flags = SS_DISABLE;
    if (sigaltstack(&alternate, nullptr)) return -1;
    fprintf(stderr, "STEAMOHOS_GNU_EXEC_V1 native_handoff image=%s generation=%u\n",
        target.substr(target.rfind('/')+1).c_str(), generation + 1);
    for (int fd : closeOnExec) close(fd);
    image.Enter();
}
int Search(const char* file, char* const argv[], char* const env[]) {
    if (!file || !*file) return Fail(ENOENT);
    if (strchr(file, '/')) return Handoff(file, argv, env);
    const char* configured = getenv("PATH");
    const std::string paths = configured ? configured : "/bin:/usr/bin";
    size_t start=0; bool denied=false;
    do {
        size_t end=paths.find(':',start);
        std::string directory=paths.substr(start,end==std::string::npos?end:end-start);
        const std::string candidate=(directory.empty()?".":directory) + '/' + file;
        Handoff(candidate.c_str(), argv, env);
        if (errno == EACCES) denied=true;
        else if (errno != ENOENT && errno != ENOTDIR) return -1;
        if (end == std::string::npos) break;
        start=end+1;
    } while (start<=paths.size());
    return Fail(denied?EACCES:ENOENT);
}
int List(const char* path, const char* first, va_list values, bool search, bool explicitEnvironment) {
    std::vector<char*> argv;
    const char* value = first;
    while (value) {
        if (argv.size()>=120) return Fail(E2BIG);
        argv.push_back(const_cast<char*>(value)); value=va_arg(values,const char*);
    }
    argv.push_back(nullptr);
    char* const* env = explicitEnvironment ? va_arg(values,char* const*) : environ;
    return search ? Search(path,argv.data(),env) : Handoff(path,argv.data(),env);
}
}
// SIGSYS is already fatal in this diagnostic backend. Keep the syscall
// number available even after Steam's Breakpad installs its own handler.
// This reports and terminates the denied operation; it never emulates success.
extern "C" int sigaction(int signal, const struct sigaction* action, struct sigaction* old) noexcept {
    if (!NextSigaction) { errno = ENOSYS; return -1; }
    const char* native = getenv("STEAMOHOS_NATIVE_MEMORY");
    if (signal == SIGSYS && action && native && !strcmp(native, "1")) {
        fprintf(stderr,"STEAMOHOS_SIGSYS_HANDLER replaced_request=1\n");
        struct sigaction diagnostic = *action;
        diagnostic.sa_flags |= SA_SIGINFO;
        diagnostic.sa_sigaction = ExecSigSys;
        return NextSigaction(signal, &diagnostic, old);
    }
    return NextSigaction(signal, action, old);
}
extern "C" int execve(const char* path,char* const argv[],char* const env[]) noexcept {
    try { return Handoff(path,argv,env); } catch (...) { return Fail(ENOMEM); }
}
extern "C" int execv(const char* path,char* const argv[]) noexcept { return execve(path,argv,environ); }
extern "C" int execvp(const char* path,char* const argv[]) noexcept {
    try { return Search(path,argv,environ); } catch (...) { return Fail(ENOMEM); }
}
extern "C" int execvpe(const char* path,char* const argv[],char* const env[]) noexcept {
    try { return Search(path,argv,env); } catch (...) { return Fail(ENOMEM); }
}
#define EXEC_LIST(name, search, explicitEnvironment) \
extern "C" int name(const char* path,const char* first,...) noexcept { \
    va_list values;va_start(values,first);int result; \
    try{result=List(path,first,values,search,explicitEnvironment);}catch(...){result=Fail(ENOMEM);} \
    va_end(values);return result; \
}
EXEC_LIST(execl,false,false)
EXEC_LIST(execlp,true,false)
EXEC_LIST(execle,false,true)

// Observe libc's internal spawn exec path without changing vfork semantics.
namespace {
int TraceSpawn(bool search,pid_t* pid,const char* path,const posix_spawn_file_actions_t* actions,
               const posix_spawnattr_t* attr,char* const argv[],char* const env[]) {
    using Spawn=int (*)(pid_t*,const char*,const posix_spawn_file_actions_t*,const posix_spawnattr_t*,char* const[],char* const[]);
    auto next=reinterpret_cast<Spawn>(dlsym(RTLD_NEXT,search?"posix_spawnp":"posix_spawn"));
    if(!next)return ENOSYS;
    short flags=0;if(attr)posix_spawnattr_getflags(attr,&flags);
    fprintf(stderr,"STEAMOHOS_SPAWN_ATTEMPT search=%d path=%s actions=%d flags=0x%x\n",
        search,path?path:"<null>",actions!=nullptr,static_cast<unsigned short>(flags));
    int result=next(pid,path,actions,attr,argv,env);
    fprintf(stderr,"STEAMOHOS_SPAWN_RETURN result=%d pid=%d\n",result,!result&&pid?*pid:-1);
    return result;
}
}
extern "C" int posix_spawn(pid_t* pid,const char* path,const posix_spawn_file_actions_t* actions,
    const posix_spawnattr_t* attr,char* const argv[],char* const env[]) noexcept {
    return TraceSpawn(false,pid,path,actions,attr,argv,env);
}
extern "C" int posix_spawnp(pid_t* pid,const char* path,const posix_spawn_file_actions_t* actions,
    const posix_spawnattr_t* attr,char* const argv[],char* const env[]) noexcept {
    return TraceSpawn(true,pid,path,actions,attr,argv,env);
}
#define TRACE_ACTION(name,params,args,format,...) \
extern "C" int name params noexcept { \
    using Function=int (*) params;auto next=reinterpret_cast<Function>(dlsym(RTLD_NEXT,#name)); \
    if(!next){return ENOSYS;}int result=next args; \
    fprintf(stderr,"STEAMOHOS_SPAWN_ACTION " #name " " format " result=%d\n",__VA_ARGS__,result);return result; \
}
TRACE_ACTION(posix_spawn_file_actions_addclose,(posix_spawn_file_actions_t* a,int fd),(a,fd),"fd=%d",fd)
TRACE_ACTION(posix_spawn_file_actions_adddup2,(posix_spawn_file_actions_t* a,int fd,int to),(a,fd,to),"fd=%d to=%d",fd,to)
TRACE_ACTION(posix_spawn_file_actions_addopen,(posix_spawn_file_actions_t* a,int fd,const char* path,int flags,mode_t mode),(a,fd,path,flags,mode),"fd=%d path=%s flags=0x%x",fd,path,flags)
TRACE_ACTION(posix_spawn_file_actions_addchdir_np,(posix_spawn_file_actions_t* a,const char* path),(a,path),"path=%s",path)
TRACE_ACTION(posix_spawn_file_actions_addfchdir_np,(posix_spawn_file_actions_t* a,int fd),(a,fd),"fd=%d",fd)
TRACE_ACTION(posix_spawn_file_actions_addclosefrom_np,(posix_spawn_file_actions_t* a,int fd),(a,fd),"fd=%d",fd)
