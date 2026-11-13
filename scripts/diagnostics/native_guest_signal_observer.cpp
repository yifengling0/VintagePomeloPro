// SPDX-License-Identifier: MIT
// Opt-in x86-64 guest diagnostic, not a production signal implementation.
// Immutable handler slots preserve oldact, masks, flags and handler context.
#define _GNU_SOURCE 1
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr unsigned Capacity=64, EventLimit=256;
struct Slot {struct sigaction action{};std::atomic<bool> ready{false};};
std::array<Slot,Capacity> slots;
std::atomic<unsigned> used{0},events{0};
using Handler=void (*)(int,siginfo_t*,void*);
using Action=int (*)(int,const struct sigaction*,struct sigaction*);
Action nextAction;
char* append(char* p,const char* text) {while(*text)*p++=*text++;return p;}
char* number(char* p,uint64_t value,unsigned radix) {
    char digits[32];unsigned n=0;
    do {digits[n++]="0123456789abcdef"[value%radix];value/=radix;} while(value);
    while(n)*p++=digits[--n];
    return p;
}
void observe(int signal,siginfo_t* info,void* context) {
    if(events.fetch_add(1,std::memory_order_relaxed)>=EventLimit)return;
    const int saved=errno;char line[256],*p=line;
    auto* state=static_cast<ucontext_t*>(context);
    p=append(p,"NATIVE_SIGNAL signal=");p=number(p,signal,10);
    p=append(p," code=0x");p=number(p,static_cast<unsigned>(info?info->si_code:0),16);
    p=append(p," pc=0x");p=number(p,state?state->uc_mcontext.gregs[REG_RIP]:0,16);
    p=append(p," address=0x");p=number(p,info?reinterpret_cast<uintptr_t>(info->si_addr):0,16);
    p=append(p," error=0x");p=number(p,state?state->uc_mcontext.gregs[REG_ERR]:0,16);
    *p++='\n';(void)!write(2,line,static_cast<size_t>(p-line));errno=saved;
}
template<size_t Index> void dispatch(int signal,siginfo_t* info,void* context) {
    if(!slots[Index].ready.load(std::memory_order_acquire))_exit(128+signal);
    observe(signal,info,context);
    const auto& original=slots[Index].action;
    if(original.sa_flags&SA_SIGINFO)original.sa_sigaction(signal,info,context);
    else original.sa_handler(signal);
}
template<size_t... Index> constexpr auto handlers(std::index_sequence<Index...>) {
    return std::array<Handler,sizeof...(Index)>{dispatch<Index>...};
}
constexpr auto callbacks=handlers(std::make_index_sequence<Capacity>{});
bool watched(int signal) {
    return signal==SIGSEGV||signal==SIGBUS||signal==SIGILL||signal==SIGFPE||signal==SIGABRT;
}
__attribute__((constructor)) void ready() {
    nextAction=reinterpret_cast<Action>(dlsym(RTLD_NEXT,"sigaction"));
    static const char text[]="NATIVE_SIGNAL observer_ready=1 scope=guest bounded_events=256\n";
    (void)!write(2,text,sizeof(text)-1);
}
}
extern "C" int sigaction(int signal,const struct sigaction* action,struct sigaction* old) {
    if(!nextAction){errno=ENOSYS;return -1;}
    struct sigaction replacement{};const struct sigaction* selected=action;
    if(action&&watched(signal)&&action->sa_handler!=SIG_DFL&&action->sa_handler!=SIG_IGN) {
        const unsigned slot=used.fetch_add(1,std::memory_order_relaxed);
        if(slot<Capacity) {
            slots[slot].action=*action;
            slots[slot].ready.store(true,std::memory_order_release);
            replacement=*action;replacement.sa_sigaction=callbacks[slot];replacement.sa_flags|=SA_SIGINFO;
            selected=&replacement;
            Dl_info module{};
            if(dladdr(reinterpret_cast<void*>(action->sa_sigaction),&module)) {
                // Registration occurs outside a signal. No loader calls or
                // unwinding occur in dispatch itself.
                char line[160],*p=line;p=append(p,"NATIVE_SIGNAL handler_base=0x");
                p=number(p,reinterpret_cast<uintptr_t>(module.dli_fbase),16);
                p=append(p," module=");
                const char* leaf=module.dli_fname?strrchr(module.dli_fname,'/'):nullptr;
                leaf=leaf?leaf+1:(module.dli_fname?module.dli_fname:"unknown");
                for(unsigned n=0;n<80&&leaf[n];++n)*p++=leaf[n];
                *p++='\n';
                (void)!write(2,line,static_cast<size_t>(p-line));
            }
        }
    }
    const int result=nextAction(signal,selected,old),saved=errno;
    if(result==0&&old&&watched(signal))for(unsigned i=0;i<Capacity;++i) {
        if(old->sa_sigaction==callbacks[i]&&slots[i].ready.load(std::memory_order_acquire)) {
            old->sa_sigaction=slots[i].action.sa_sigaction;
            old->sa_flags=(old->sa_flags&~SA_SIGINFO)|(slots[i].action.sa_flags&SA_SIGINFO);
            break;
        }
    }
    errno=saved;return result;
}
