// Native GNU exec handoff for installed or verified external research images. No CPU translation.
#include "../native_loader.h"
#include "native_fex.h"
#include "native_signal_context.h"
#include "native_seccomp.h"
#include "native_executable.h"
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
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/auxv.h>
#include <vector>

extern char** environ;
extern "C" __attribute__((weak,visibility("hidden"))) long (*SteamOhosNextSyscall)(long,...);
namespace {
using KernelExec = int (*)(const char*, char* const[], char* const[]);
KernelExec NextExec = nullptr;
using KernelSigaction = int (*)(int, const struct sigaction*, struct sigaction*);
KernelSigaction NextSigaction = nullptr;
using KernelWaitPid = pid_t (*)(pid_t, int*, int);
using KernelWaitId = int (*)(idtype_t, id_t, siginfo_t*, int);
using KernelKill = int (*)(pid_t, int);
KernelWaitPid NextWaitPid = nullptr;
KernelWaitId NextWaitId = nullptr;
KernelKill NextKill = nullptr;
unsigned long FexMainPhdr = 0;
bool FexSignalDiagnostics = false;
bool FexNativeScope = false;
bool ProcessDiagnostics() {
    const char* native = getenv("STEAMOHOS_NATIVE_MEMORY");
    return native && !strcmp(native, "1");
}
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
__attribute__((no_stack_protector)) void ExecSigSys(int, siginfo_t* info, void* context) {
    if(FexNativeScope && steamohos::fex::UnsupportedFileHandleCall(info,context)) {
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<8) {
            static const char message[]="STEAMOHOS_FEX_OPTIONAL file_handle_api=ENOSYS\n";
            RawCall(SYS_write,2,reinterpret_cast<long>(message),sizeof(message)-1);
        }
        return;
    }
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
// FEX chains its unhandled fault to the previous action. Record only machine
// state, then restore the real fatal signal; never consume a guest exception.
__attribute__((no_stack_protector)) void RecordFexFault(const char* label,int signal,siginfo_t* info,void* context,
        const stack_t* registered=nullptr,long query=-1,bool repaired=false) {
    char message[512];size_t used=0;
    auto literal=[&](const char* value){while(*value&&used<sizeof(message)-1)message[used++]=*value++;};
    auto number=[&](unsigned long value){
        static const char digits[]="0123456789abcdef";char reverse[2*sizeof(value)];unsigned count=0;
        do{reverse[count++]=digits[value&15];value>>=4;}while(value&&count<sizeof(reverse));
        while(count&&used<sizeof(message)-1)message[used++]=reverse[--count];
    };
    unsigned long pc=0,lr=0,sp=0;
    if(context) {
        auto* state=static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
        pc=state->uc_mcontext.pc;lr=state->uc_mcontext.regs[30];sp=state->uc_mcontext.sp;
#elif defined(__x86_64__)
        pc=state->uc_mcontext.gregs[REG_RIP];sp=state->uc_mcontext.gregs[REG_RSP];
#endif
    }
    literal(label);literal(" signal=0x");number(signal);
    literal(" code=0x");number(info?static_cast<unsigned>(info->si_code):0);
    literal(" address=0x");number(info?reinterpret_cast<unsigned long>(info->si_addr):0);
    literal(" pc=0x");number(pc);literal(" lr=0x");number(lr);literal(" sp=0x");number(sp);
    literal(" main_phdr=0x");number(FexMainPhdr);
#if defined(__aarch64__)
    if(context){literal(" x20=0x");number(static_cast<ucontext_t*>(context)->uc_mcontext.regs[20]);}
#endif
    if(context){literal(" alt_sp=0x");number(reinterpret_cast<unsigned long>(static_cast<ucontext_t*>(context)->uc_stack.ss_sp));}
    literal(" alt_flags=0x");number(context?static_cast<unsigned>(static_cast<ucontext_t*>(context)->uc_stack.ss_flags):0);
    if(registered) {
        literal(" kernel_alt_sp=0x");number(reinterpret_cast<unsigned long>(registered->ss_sp));
        literal(" kernel_alt_size=0x");number(registered->ss_size);
        literal(" kernel_alt_flags=0x");number(static_cast<unsigned>(registered->ss_flags));
        literal(" kernel_alt_query=0x");number(static_cast<unsigned long>(query));
        literal(" context_repaired=0x");number(repaired);
    }
    if(signal==SIGSYS){literal(" syscall=0x");number(info?static_cast<unsigned>(info->si_syscall):0);
#if defined(__aarch64__)
        if(context){auto* state=static_cast<ucontext_t*>(context);literal(" a0=0x");number(state->uc_mcontext.regs[0]);literal(" a1=0x");number(state->uc_mcontext.regs[1]);}
#endif
    }
    literal("\n");
    RawCall(SYS_write,2,reinterpret_cast<long>(message),used);
}
// Diagnostic only: bounded, raw-syscall evidence survives a damaged GNU TLS.
__attribute__((no_stack_protector)) void FexFaultCode(void* context) {
    (void)context;
#if defined(__aarch64__)
    if(!context)return;
    auto* state=static_cast<ucontext_t*>(context);
    static const char digits[]="0123456789abcdef";
    char line[1024];size_t used=0;
    auto text=[&](const char* s){while(*s&&used<sizeof(line)-1)line[used++]=*s++;};
    auto hex=[&](uint64_t value){for(int n=15;n>=0;--n)line[used++]=digits[(value>>(n*4))&15];};
    text("STEAMOHOS_FEX_FAULT_REGS");
    for(unsigned i=0;i<31;++i){text(" x");line[used++]='0'+i/10;line[used++]='0'+i%10;line[used++]='=';hex(state->uc_mcontext.regs[i]);}
    line[used++]='\n';RawCall(SYS_write,2,reinterpret_cast<long>(line),used);
    // /proc/self/mem is denied on some OHOS kernels. The kernel validates
    // process_vm_readv ranges without dereferencing an unsafe fault pointer.
    // A null indirect branch has no bytes at PC; its call site is still in LR.
    const uint64_t instruction=state->uc_mcontext.pc?state->uc_mcontext.pc:state->uc_mcontext.regs[30];
    uint64_t start=instruction>=64?instruction-64:0;
    unsigned char bytes[192];
    iovec local{bytes,sizeof(bytes)},remote{reinterpret_cast<void*>(start),sizeof(bytes)};
    const long self=RawCall(SYS_getpid,0,0,0);
    register long x8 asm("x8")=SYS_process_vm_readv;
    register long x0 asm("x0")=self;
    register long x1 asm("x1")=reinterpret_cast<long>(&local);
    register long x2 asm("x2")=1;
    register long x3 asm("x3")=reinterpret_cast<long>(&remote);
    register long x4 asm("x4")=1;
    register long x5 asm("x5")=0;
    asm volatile("svc 0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5) : "memory","cc");
    const long count=x0;
    if(count<=0){used=0;text("STEAMOHOS_FEX_FAULT_CODE read_result=");hex(count);text("\n");RawCall(SYS_write,2,reinterpret_cast<long>(line),used);return;}
    used=0;text("STEAMOHOS_FEX_FAULT_CODE address=");hex(start);text(" bytes=");
    for(long n=0;n<count;++n){line[used++]=digits[bytes[n]>>4];line[used++]=digits[bytes[n]&15];}
    line[used++]='\n';RawCall(SYS_write,2,reinterpret_cast<long>(line),used);
#if defined(STEAMOHOS_NATIVE_FAULT_ORIGIN)
    // Capture the translated direct-call destination as well as its caller.
    // A BL followed by a null PC does not identify which instruction failed.
    // Read only this process through the kernel; never dereference JIT state.
    auto slice=[&](const char* label,uint64_t address,size_t length)->uint64_t {
        unsigned char data[512];
        if(!address||length>sizeof(data))return 0;
        iovec dst{data,length},src{reinterpret_cast<void*>(address),length};
        register long n asm("x8")=SYS_process_vm_readv;
        register long a0 asm("x0")=self;
        register long a1 asm("x1")=reinterpret_cast<long>(&dst);
        register long a2 asm("x2")=1;
        register long a3 asm("x3")=reinterpret_cast<long>(&src);
        register long a4 asm("x4")=1;
        register long a5 asm("x5")=0;
        asm volatile("svc 0" : "+r"(a0) : "r"(n),"r"(a1),"r"(a2),"r"(a3),"r"(a4),"r"(a5) : "memory","cc");
        const long readCount=a0;
        used=0;text(label);text(" address=");hex(address);text(" result=");hex(readCount);text("\n");
        RawCall(SYS_write,2,reinterpret_cast<long>(line),used);
        if(readCount>0) {
            // Each write remains below the fixed log-line buffer capacity.
            for(long offset=0;offset<readCount;offset+=192) {
                used=0;text(label);text(" offset=");hex(offset);text(" bytes=");
                const long end=std::min(readCount,offset+192);
                for(long i=offset;i<end;++i){line[used++]=digits[data[i]>>4];line[used++]=digits[data[i]&15];}
                line[used++]='\n';RawCall(SYS_write,2,reinterpret_cast<long>(line),used);
            }
        }
        uint64_t first=0;
        if(readCount>=8)for(unsigned i=0;i<8;++i)first|=static_cast<uint64_t>(data[i])<<(i*8);
        return first;
    };
    const uint64_t lr=state->uc_mcontext.regs[30];
    if(lr>=start+4&&lr<=start+static_cast<uint64_t>(count)) {
        const size_t offset=lr-start-4;
        const uint32_t call=static_cast<uint32_t>(bytes[offset])|
            (static_cast<uint32_t>(bytes[offset+1])<<8)|
            (static_cast<uint32_t>(bytes[offset+2])<<16)|
            (static_cast<uint32_t>(bytes[offset+3])<<24);
        if((call&0xfc000000u)==0x94000000u) {
            const int64_t displacement=static_cast<int64_t>(static_cast<int32_t>((call&0x03ffffffu)<<6))>>4;
            slice("STEAMOHOS_FEX_CALL_TARGET",lr-4+displacement,512);
        }
    }
    slice("STEAMOHOS_FEX_CALL_STACK",state->uc_mcontext.regs[25],64);
    const uint64_t block=slice("STEAMOHOS_FEX_STATE",state->uc_mcontext.regs[28],256);
    // The saved CPU-state head identifies the most recent translated block,
    // which may differ from the stale host LR after a guest RET. Capture it
    // through process_vm_readv, never by dereferencing signal-time JIT state.
    slice("STEAMOHOS_FEX_CURRENT_BLOCK",block,512);
    for(unsigned offset=512;block&&offset<4096;offset+=512)
        slice("STEAMOHOS_FEX_CURRENT_BLOCK_NEXT",block+offset,512);
    // This Valve FEX ABI holds guest RSP/RBP in x8/x9. Keep the capture opt-in
    // and bounded; an unreadable pointer is evidence, not a second fault.
    const uint64_t guestSp=state->uc_mcontext.regs[8],guestBp=state->uc_mcontext.regs[9];
    if(guestSp>=128)slice("STEAMOHOS_FEX_GUEST_STACK",guestSp-128,512);
    if(guestBp>=128)slice("STEAMOHOS_FEX_GUEST_FRAME",guestBp-128,256);
#endif
#endif
}
#if defined(STEAMOHOS_NATIVE_FAULT_ORIGIN) && defined(__aarch64__)
__attribute__((no_stack_protector)) void FexOriginLocation(void* context) {
    if(!context)return;
    const uintptr_t address=static_cast<ucontext_t*>(context)->uc_mcontext.regs[30];
    long fd=RawCall(SYS_openat,AT_FDCWD,reinterpret_cast<long>("/proc/self/maps"),O_RDONLY|O_CLOEXEC);
    if(fd<0)return;
    char page[4096],line[1024];size_t used=0,total=0;bool found=false;
    auto hex=[](const char*& p) {
        uintptr_t value=0;
        while((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')) {
            value=(value<<4)|(*p<='9'?*p-'0':*p-'a'+10);++p;
        }
        return value;
    };
    // Scan bounded metadata, emitting only the map containing the call site.
    while(!found&&total<4*1024*1024) {
        long count=RawCall(SYS_read,fd,reinterpret_cast<long>(page),sizeof(page));
        if(count<=0)break;
        total+=count;
        for(long i=0;i<count;++i) {
            if(page[i]!='\n'){if(used<sizeof(line)-2)line[used++]=page[i];continue;}
            line[used]=0;const char* p=line;const uintptr_t begin=hex(p);
            if(*p++=='-') {
                const uintptr_t end=hex(p);
                if(begin<=address&&address<end) {
                    static const char label[]="STEAMOHOS_FEX_ORIGIN_MAP ";
                    RawCall(SYS_write,2,reinterpret_cast<long>(label),sizeof(label)-1);
                    line[used++]='\n';RawCall(SYS_write,2,reinterpret_cast<long>(line),used);
                    found=true;break;
                }
            }
            used=0;
        }
    }
    RawCall(SYS_close,fd,0,0);
}
#endif
__attribute__((no_stack_protector)) void FexFaultMaps() {
    static const char begin[]="STEAMOHOS_FEX_FAULT_MAPS_BEGIN\n";
    static const char end[]="STEAMOHOS_FEX_FAULT_MAPS_END\n";
    long descriptor=RawCall(SYS_openat,AT_FDCWD,reinterpret_cast<long>("/proc/self/maps"),O_RDONLY|O_CLOEXEC);
    if(descriptor<0)return;
    RawCall(SYS_write,2,reinterpret_cast<long>(begin),sizeof(begin)-1);
    char bytes[4096];size_t total=0;
    while(total<96*1024) {
        long count=RawCall(SYS_read,descriptor,reinterpret_cast<long>(bytes),sizeof(bytes));
        if(count<=0)break;
        RawCall(SYS_write,2,reinterpret_cast<long>(bytes),count);total+=count;
    }
    RawCall(SYS_close,descriptor,0,0);
    RawCall(SYS_write,2,reinterpret_cast<long>(end),sizeof(end)-1);
}
__attribute__((no_stack_protector)) void FexTerminalFault(int signal,siginfo_t* info,void* context) {
    RecordFexFault("STEAMOHOS_FEX_TERMINAL_FAULT",signal,info,context);
    FexFaultCode(context);
    FexFaultMaps();
    struct sigaction reset{};reset.sa_handler=SIG_DFL;
    if(NextSigaction)NextSigaction(signal,&reset,nullptr);
    RawCall(SYS_tgkill,RawCall(SYS_getpid,0,0,0),RawCall(SYS_gettid,0,0,0),signal);
}


// FEX uses the kernel sigaction ABI and recovers its thread object from
// uc_stack. Some OHOS kernels omit that field despite a live registered stack.
// Repair only a wholly empty field from this thread's real kernel stack.
// Preserve the original flags/mask/restorer, handler queries and fatal signals.
std::atomic<uintptr_t> FexObservedHandlers[NSIG]{};
__attribute__((no_stack_protector)) void FexObservedSignal(int signal,siginfo_t* info,void* context) {
    stack_t current{};
    const long result=RawCall(SYS_sigaltstack,0,reinterpret_cast<long>(&current),0);
    bool repaired=false;
    if(!result&&context) {
        auto* state=static_cast<ucontext_t*>(context);
        uintptr_t interrupted=0;
#if defined(__aarch64__)
        interrupted=state->uc_mcontext.sp;
#elif defined(__x86_64__)
        interrupted=state->uc_mcontext.gregs[REG_RSP];
#endif
        repaired=steamohos::fex::RepairEmptySignalStack(state->uc_stack,current,interrupted);
    }
    static std::atomic<unsigned> reports{0};
    if(FexSignalDiagnostics&&reports.fetch_add(1,std::memory_order_relaxed)<24)
        RecordFexFault("STEAMOHOS_FEX_SIGNAL",signal,info,context,&current,result,repaired);
#if defined(STEAMOHOS_NATIVE_FAULT_ORIGIN)
    // An exception inside a native thunk or syscall can leave the last guest
    // PC at an unrelated instruction. Capture the actual host context before
    // FEX reconstructs it. Keep this out of release builds and independent of
    // the normal alignment-fault budget; do not consume or alter the signal.
    static std::atomic<unsigned> origins{0};
    if(FexSignalDiagnostics&&signal==SIGSEGV&&info&&info->si_code==SEGV_MAPERR) {
        const unsigned index=origins.fetch_add(1,std::memory_order_relaxed);
        if(index<8) {
            RecordFexFault("STEAMOHOS_FEX_ORIGIN",signal,info,context,&current,result,repaired);
            FexFaultCode(context);
#if defined(__aarch64__)
            FexOriginLocation(context);
#endif
            if(!index)FexFaultMaps();
        }
    }
#endif
    auto handler=reinterpret_cast<void(*)(int,siginfo_t*,void*)>(FexObservedHandlers[signal].load(std::memory_order_relaxed));
    if(handler)handler(signal,info,context);
}

__attribute__((constructor)) void InitializeExec() {
    NextExec = reinterpret_cast<KernelExec>(dlsym(RTLD_NEXT, "execve"));
    NextSigaction = reinterpret_cast<KernelSigaction>(dlsym(RTLD_NEXT, "sigaction"));
    NextWaitPid = reinterpret_cast<KernelWaitPid>(dlsym(RTLD_NEXT, "waitpid"));
    NextWaitId = reinterpret_cast<KernelWaitId>(dlsym(RTLD_NEXT, "waitid"));
    NextKill = reinterpret_cast<KernelKill>(dlsym(RTLD_NEXT, "kill"));
    if (NextSigaction && getenv("STEAMOHOS_EXEC_BOOTSTRAP_DIAGNOSTIC")) {
        struct sigaction reset{}; reset.sa_handler = SIG_DFL; sigemptyset(&reset.sa_mask);
        (void)NextSigaction(SIGSEGV, &reset, nullptr);
        unsetenv("STEAMOHOS_EXEC_BOOTSTRAP_DIAGNOSTIC");
    }
    const char* native=getenv("STEAMOHOS_NATIVE_MEMORY");
    const char* fex=getenv("STEAMOHOS_NATIVE_FEX");
    FexNativeScope=fex&&!strcmp(fex,"1");
    FexSignalDiagnostics=native&&!strcmp(native,"1")&&FexNativeScope;
    if(NextSigaction&&native&&!strcmp(native,"1")&&fex&&!strcmp(fex,"1")) {
        FexMainPhdr=getauxval(AT_PHDR);
        struct sigaction terminal{};terminal.sa_flags=SA_SIGINFO;
        terminal.sa_sigaction=FexTerminalFault;sigemptyset(&terminal.sa_mask);
        (void)NextSigaction(SIGSEGV,&terminal,nullptr);
    }
    if (NextSigaction && (FexNativeScope || (native && !strcmp(native,"1")))) {
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
    // Exact official FEX entry only; never allow arbitrary depot scripts.
    if (!steamohos::fex::Tool(path).empty()) return code+"/steamohos_fex";
    if (steamohos::fex::Helper(path)) return steamohos::fex::Canonical(path);
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
    unsigned char header[64]; ssize_t bytes = read(input, header, sizeof(header)); close(input);
#if defined(__aarch64__)
    const std::string loader = code + "/ld-linux-aarch64.so.1";
#elif defined(__x86_64__)
    const std::string loader = code + "/ld-linux-x86-64.so.2";
#else
#error Unsupported GNU native ISA
#endif
    if (bytes<0||!SteamOhosNativeExecutableHeader(header,static_cast<size_t>(bytes)))return Fail(ENOEXEC);
    std::vector<std::string> argv{loader, "--inhibit-cache", "--library-path",
        code + ':' + code + "/steamrtarm64:" + code + "/steamrtarm64/libs:" + code + "/steamrtarm64/video",
        "--argv0", arguments && arguments[0] ? arguments[0] : target, target};
    if (arguments) for (size_t n=1; arguments[n]; ++n) {
        if (n >= 120 || strnlen(arguments[n], 16384) >= 16384) return Fail(E2BIG);
        argv.emplace_back(arguments[n]);
    }
    // Native FEX host thunks use the already delivered GNU graphics/audio
    // closure, while Steam and Windows GNU Wine retain their original paths.
    if (steamohos::fex::Helper(target)) {
        const std::string graphics=steamohos::fex::Env("STEAMOHOS_NATIVE_GRAPHICS_ROOT");
        if(graphics=="/data/storage/el2/base/files/steamohos-native/proton-probe")
            // Keep the paired GNU C/C++/Wayland/audio closure first. The
            // graphics root also contains another libstdc++/libc copy, which
            // must not replace this process's already selected runtime.
            argv[3]=code+"/native-vulkan-driver:"+code+":"+graphics+"/graphics:"+graphics+":"+argv[3].substr(code.size()+1);
    }
    std::vector<std::string> environment;
    if (supplied) for (size_t n=0; supplied[n]; ++n) {
        if (n >= 220 || strnlen(supplied[n], 16384) >= 16384) return Fail(E2BIG);
        environment.emplace_back(supplied[n]);
    }
    for (const char* key : {"STEAMOHOS_CODE_ROOT", "STEAMOHOS_STEAM_ROOT", "STEAMOHOS_TMP_ROOT",
            "STEAMOHOS_LAUNCH_SOCKET", "STEAMOHOS_LAUNCH_NONCE", "STEAMOHOS_Z_ROOT",
            "STEAMOHOS_WINE_PREFIX", "STEAMOHOS_GNU_WINE_PREFIX", "STEAMOHOS_LIBRARY_ROOT",
            "STEAMOHOS_NATIVE_GUEST_CORE", "STEAMOHOS_NATIVE_GRAPHICS_ROOT", "STEAMOHOS_NATIVE_FEX", "STEAMOHOS_NATIVE_FEX_TOOL", "STEAMOHOS_NATIVE_FEX_ROOTFS", "STEAMOHOS_NATIVE_FEX_IPC", "FEX_SERVERSOCKETPATH",
            "FEX_PORTABLE", "FEX_ROOTFS", "FEX_APP_CONFIG_LOCATION", "FEX_APP_DATA_LOCATION", "FEX_APP_CONFIG", "FEX_THUNKHOSTLIBS", "FEX_THUNKGUESTLIBS", "FEX_X87REDUCEDPRECISION",
            "STEAMOHOS_AUDIO_BOOTSTRAP_FD", "STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET", "ALSA_CONFIG_PATH", "ALSA_PLUGIN_DIR",
            "SDL_AUDIO_DRIVER", "STEAMOHOS_GAMEPAD_SOCKET",
            "VN_DEBUG", "VN_PERF", "VN_WINEHUA_STRONG_RING_BARRIER", "VN_WINEHUA_REMOTE_MEMORY_SYNC",
            "VN_WINEHUA_PERSISTENT_MAP_SYNC", "VN_WINEHUA_DIRECT_FENCE_WAIT", "VN_OHOS_NATIVE_MEMORY",
            "STEAMOHOS_IPC_ROOT", "STEAMOHOS_SYSVIPC_ROOT", "STEAMOHOS_SOCKET_METADATA_ROOT", "STEAMOHOS_X11_SOCKET", "STEAMOHOS_NATIVE_X11_DISPLAY", "XLOCALEDIR", "GLIBC_TUNABLES", "SSL_CERT_FILE",
            "STEAMOHOS_NATIVE_MEMORY", "STEAMOHOS_EXTERNAL_RUNTIME", "STEAMOHOS_SOFTWARE_GL", "GALLIUM_DRIVER", "LIBGL_ALWAYS_SOFTWARE", "VTEST_SOCKET_NAME", "XLIB_NO_SHM", "STEAMOHOS_SOCKET_METADATA"}) {
        const char* value = getenv(key); if (value) Set(environment, key, value);
    }
    const char* software=getenv("STEAMOHOS_SOFTWARE_GL");
    const char* gamepad=getenv("STEAMOHOS_GAMEPAD_SOCKET");
    const std::string posix=code+(!access((code+"/libsteamohos_posix_hap.so").c_str(),R_OK)?"/libsteamohos_posix_hap.so":"/libsteamohos_posix.so");
    const auto repairedSoftware=code+"/native-game-support/libsteamohos_software_gl.so";
    const auto softwareLibrary=access(repairedSoftware.c_str(),R_OK)?code+"/libsteamohos_software_gl.so":repairedSoftware;
    const std::string preload=posix+
        (software && !strcmp(software,"1") ? ":"+softwareLibrary : "")+
        (gamepad && *gamepad ? ":"+code+"/libsteamohos_gamepad.so" : "");
    if (steamohos::fex::Helper(target)) {
        // FEX copies its main envp into the x86 process, then APPENDS Config.Env.
        // Loading ARM64 adapters via LD_PRELOAD would therefore leak them into
        // the guest; appending an empty value does not override the first one.
        // GNU rtld --preload applies to this host image only. Its --library-path
        // above resolves host dependencies while envp keeps guest search paths.
        argv.insert(argv.begin()+4,{"--preload",preload});
        // Remove these entries, including empty ones. FEX appends guest Env
        // after the inherited array, and glibc takes its first LD_PRELOAD.
        // An inherited empty entry would suppress the guest IPC adapter.
        environment.erase(std::remove_if(environment.begin(),environment.end(),[](const std::string& entry) {
            return entry.compare(0,11,"LD_PRELOAD=")==0 || entry.compare(0,9,"LD_AUDIT=")==0;
        }),environment.end());
    } else Set(environment,"LD_PRELOAD",preload);
    Set(environment, "STEAMOHOS_EXEC_PATH", target);
    Set(environment, "STEAMOHOS_EXEC_GENERATION", std::to_string(generation + 1));
    if (ProcessDiagnostics()) Set(environment, "STEAMOHOS_EXEC_BOOTSTRAP_DIAGNOSTIC", "1");
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
    // process. Prepare reclamation while libc/TLS are still valid; the final
    // raw-syscall trampoline runs entirely outside the previous image.
    struct sigaction action{}; action.sa_handler = SIG_DFL; sigemptyset(&action.sa_mask);
    for (int signal=1; signal<NSIG; ++signal) {
        if (signal == SIGKILL || signal == SIGSTOP) continue;
        struct sigaction old{};
        if (!sigaction(signal, nullptr, &old) && old.sa_handler != SIG_IGN)
            if (sigaction(signal, &action, nullptr) && errno != EINVAL) return -1;
    }
    // The old handler's code will be unmapped too. Leave SIGSYS at SIG_DFL
    // until the new preload installs a handler backed by its own live image.
    action.sa_flags = 0;
    action.sa_handler = SIG_DFL;
    if (!NextSigaction || NextSigaction(SIGSYS, &action, nullptr)) return -1;
    stack_t alternate{}; alternate.ss_flags = SS_DISABLE;
    if (sigaltstack(&alternate, nullptr)) return -1;
    if (!image.PrepareExecCleanup()) {
        fprintf(stderr, "STEAMOHOS_GNU_EXEC_V1 reclaim_failed stage=%s errno=%d\n", image.stage.c_str(), image.error);
        return Fail(image.error);
    }
    if (ProcessDiagnostics()) fputs(image.diagnostics.c_str(), stderr);
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
// Filesystem preflight must use exactly the exec handoff's configured namespace.
// No fallback for external depot programs, scripts or unregistered helpers.
extern "C" int SteamOhosResolveUserExecutable(const char* filename,char* resolved,size_t capacity) {
    const char* configured=getenv("STEAMOHOS_CODE_ROOT");
    const std::string code=configured?configured:"";
    const std::string target=Executable(filename,code);
    if(!Under(target,code)||target.size()>=capacity)return 0;
    memcpy(resolved,target.c_str(),target.size()+1);return 1;
}
// Native FEX signal ABI compatibility is independent of diagnostic logging.
// Only SA_SIGINFO+SA_ONSTACK handlers need the stack-context repair. During
// diagnostics also observe fatal signals without changing their disposition.
extern "C" __attribute__((visibility("hidden"))) long SteamOhosNativeFexSigaction(
        long number,long signal,long action,long old,long size,long e,long f) {
    auto next=&SteamOhosNextSyscall?SteamOhosNextSyscall:nullptr;
    if(!next){errno=ENOSYS;return -1;}
    const char* native=getenv("STEAMOHOS_NATIVE_FEX");
    if(signal<=0||signal>=NSIG||!native||strcmp(native,"1"))
        return next(number,signal,action,old,size,e,f);
    struct KernelAction {uintptr_t handler,flags,restorer;uint64_t mask;};
    KernelAction observed{};uintptr_t previous=FexObservedHandlers[signal].load(std::memory_order_relaxed);
    bool wrap=false;
    if(action&&size==8) {
        observed=*reinterpret_cast<const KernelAction*>(action);
        const bool diagnostic=ProcessDiagnostics()&&(signal==SIGSEGV||signal==SIGSYS||signal==SIGBUS);
        wrap=observed.handler>1&&(observed.flags&SA_SIGINFO)&&((observed.flags&SA_ONSTACK)||diagnostic);
        if(wrap) {
            FexObservedHandlers[signal].store(observed.handler,std::memory_order_relaxed);
            observed.handler=reinterpret_cast<uintptr_t>(FexObservedSignal);
        }
    }
    long result=next(number,signal,wrap?reinterpret_cast<long>(&observed):action,old,size,e,f);
    int saved=errno;
    if(result<0&&wrap)FexObservedHandlers[signal].store(previous,std::memory_order_relaxed);
    if(result==0&&old) {
        auto returned=reinterpret_cast<KernelAction*>(old);
        if(returned->handler==reinterpret_cast<uintptr_t>(FexObservedSignal))returned->handler=previous;
    }
    errno=saved;return result;
}

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

// Observe real kernel child status without reaping extra children or changing
// a caller's wait flags. Keep this bounded and free of argv/account data.
extern "C" pid_t waitpid(pid_t pid, int* status, int options) {
    if (!NextWaitPid) { errno = ENOSYS; return -1; }
    pid_t result = NextWaitPid(pid, status, options);
    int saved = errno;
    static std::atomic<unsigned> records{0};
    if (result > 0 && status && ProcessDiagnostics() && records.fetch_add(1) < 256)
        fprintf(stderr, "STEAMOHOS_CHILD_STATUS parent=%d child=%d api=waitpid status=%d exited=%d code=%d signaled=%d signal=%d\n",
            getpid(), result, *status, WIFEXITED(*status), WIFEXITED(*status) ? WEXITSTATUS(*status) : -1,
            WIFSIGNALED(*status), WIFSIGNALED(*status) ? WTERMSIG(*status) : 0);
    errno = saved; return result;
}
extern "C" int waitid(idtype_t type, id_t id, siginfo_t* info, int options) {
    if (!NextWaitId) { errno = ENOSYS; return -1; }
    int result = NextWaitId(type, id, info, options);
    int saved = errno;
    static std::atomic<unsigned> records{0};
    if (!result && info && info->si_pid > 0 && ProcessDiagnostics() && records.fetch_add(1) < 256)
        fprintf(stderr, "STEAMOHOS_CHILD_STATUS parent=%d child=%d api=waitid reason=%d status=%d options=%d\n",
            getpid(), info->si_pid, info->si_code, info->si_status, options);
    errno = saved; return result;
}
extern "C" int kill(pid_t pid, int signal) noexcept {
    if (!NextKill) { errno = ENOSYS; return -1; }
    int result = NextKill(pid, signal);
    int saved = errno;
    static std::atomic<unsigned> records{0};
    if (signal && ProcessDiagnostics() && records.fetch_add(1) < 128)
        fprintf(stderr, "STEAMOHOS_PROCESS_SIGNAL caller=%d target=%d signal=%d result=%d errno=%d\n",
            getpid(), pid, signal, result, result < 0 ? saved : 0);
    errno = saved; return result;
}
