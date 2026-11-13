/* SPDX-License-Identifier: MIT
 * Diagnostic launcher for an ELF PlayerMain entry, with transparent observation
 * of its SA_SIGINFO SIGSEGV handler. Does not change engine code or assets.
 * Captures only fault context and bounded self-memory; never the environment.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

static int (*next_action)(int,const struct sigaction *,struct sigaction *);
static void (*observed[NSIG])(int,siginfo_t *,void *);
static volatile sig_atomic_t reports;
static void emit(const void *data,size_t bytes) {
    ssize_t result=write(2,data,bytes);(void)result;
}
#if defined(STEAMOHOS_STACK_WRITE_OBSERVER)
#include "native_stack_write_observer.h"
#endif
static char *hex(char *out,unsigned long value) {
    static const char digits[]="0123456789abcdef";
    char reverse[16];unsigned count=0;
    do{reverse[count++]=digits[value&15];value>>=4;}while(value);
    while(count)*out++=reverse[--count];return out;
}
static void word(const char *name,unsigned long value) {
    char line[128],*end=stpcpy(line,"GUEST_FAULT ");
    end=stpcpy(end,name);*end++='=';end=hex(end,value);*end++='\n';
    emit(line,(size_t)(end-line));
}
static void memory(const char *name,uintptr_t address,size_t count) {
    unsigned char data[1024];if(count>sizeof(data))count=sizeof(data);
    struct iovec local={data,count},remote={(void *)address,count};
    long bytes=syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0);
    word(name,address);
    if(bytes<=0){word("read_errno",errno);return;}
    char text[sizeof(data)*2+1];static const char digits[]="0123456789abcdef";
    for(long i=0;i<bytes;i++){text[i*2]=digits[data[i]>>4];text[i*2+1]=digits[data[i]&15];}
    text[bytes*2]='\n';emit(text,(size_t)bytes*2+1);
}
#if defined(STEAMOHOS_SIGNAL_FRAMES)
#include "native_guest_signal_frames.h"
#endif
static void selected_maps(const uintptr_t *addresses,size_t count) {
    int fd=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);if(fd<0)return;
    char page[4096],line[4096];size_t used=0,total=0,emitted=0;ssize_t bytes;
    emit("GUEST_FAULT selected_maps_begin\n",32);
    while(total<4*1024*1024&&emitted<32768&&(bytes=read(fd,page,sizeof(page)))>0) {
        total+=(size_t)bytes;
        for(ssize_t i=0;i<bytes;i++) {
            if(page[i]!='\n'){if(used<sizeof(line)-2)line[used++]=page[i];continue;}
            line[used]=0;unsigned long low=0,high=0;int match=0;
            if(sscanf(line,"%lx-%lx",&low,&high)==2)for(size_t j=0;j<count;j++)
                if(addresses[j]>=low&&addresses[j]<high)match=1;
            if(match){line[used++]='\n';emit(line,used);emitted+=used;}
            used=0;
        }
    }
    close(fd);emit("GUEST_FAULT selected_maps_end\n",30);
}
static void observe(int signal,siginfo_t *info,void *raw) {
    int saved=errno;
#if defined(STEAMOHOS_SIGNAL_FRAMES)
    signal_frame_record(signal,raw);
#endif
    if(signal==SIGSEGV&&reports++<2) {
        const ucontext_t *context=raw;
        word("signal",signal);word("code",(unsigned)info->si_code);
        word("address",(uintptr_t)info->si_addr);
        static const int indices[]={REG_RAX,REG_RBX,REG_RCX,REG_RDX,REG_RSI,REG_RDI,
            REG_RBP,REG_RSP,REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15,REG_RIP};
        static const char *names[]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp",
            "r8","r9","r10","r11","r12","r13","r14","r15","rip"};
        for(unsigned i=0;i<sizeof(indices)/sizeof(indices[0]);i++)word(names[i],context->uc_mcontext.gregs[indices[i]]);
        word("tid",syscall(SYS_gettid));
        memory("instruction",context->uc_mcontext.gregs[REG_RIP],64);
        memory("frame",context->uc_mcontext.gregs[REG_RBP]-128,256);
        memory("stack",context->uc_mcontext.gregs[REG_RSP],1024);
        memory("rax_memory",context->uc_mcontext.gregs[REG_RAX],128);
        memory("rdi_memory",context->uc_mcontext.gregs[REG_RDI],128);
        const uintptr_t locations[]={(uintptr_t)info->si_addr,
            context->uc_mcontext.gregs[REG_RIP],context->uc_mcontext.gregs[REG_RSP],
            context->uc_mcontext.gregs[REG_RBP],context->uc_mcontext.gregs[REG_RDI],
            context->uc_mcontext.gregs[REG_RBX]};
        selected_maps(locations,sizeof(locations)/sizeof(locations[0]));
#if defined(STEAMOHOS_SIGNAL_FRAMES)
        signal_frame_dump();
#endif
    }
    errno=saved;observed[signal](signal,info,raw);
}
/* Forward handler queries and preserve masks, flags and restorers verbatim. */
int sigaction(int signal,const struct sigaction *action,struct sigaction *old) {
    if(!next_action)next_action=dlsym(RTLD_NEXT,"sigaction");
    if(!next_action){errno=ENOSYS;return -1;}
    if(signal<=0||signal>=NSIG)return next_action(signal,action,old);
    struct sigaction copy;const struct sigaction *request=action;
    void (*previous)(int,siginfo_t *,void *)=observed[signal];
    int use=(signal==SIGSEGV
#if defined(STEAMOHOS_SIGNAL_FRAMES)
        ||(signal>0&&signal<NSIG)
#endif
        )&&action&&(action->sa_flags&SA_SIGINFO)&&
        (uintptr_t)action->sa_sigaction>1;
    if(use){copy=*action;copy.sa_sigaction=observe;request=&copy;}
    int rc=next_action(signal,request,old);
    if(!rc&&signal>0&&signal<NSIG){
        if(old&&old->sa_sigaction==observe)old->sa_sigaction=previous;
        if(action)observed[signal]=use?action->sa_sigaction:NULL;
    }
    return rc;
}
int main(int argc,char **argv) {
    if(argc<1)return 2;
#if defined(STEAMOHOS_DIAGNOSTIC_CONFIG)
    /* Explicit diagnostic only: keep the user's existing config untouched. */
    if(setenv("XDG_CONFIG_HOME",STEAMOHOS_DIAGNOSTIC_CONFIG,1))return 2;
    static const char config_ready[]="GUEST_CONFIG isolated=1 original_config_modified=0\n";
    emit(config_ready,sizeof(config_ready)-1);
#endif
#if defined(STEAMOHOS_SIGNAL_FRAMES)
    static const char ready[]="GUEST_SIGNAL_FRAME observer_ready=1 history=64 mutations=0\n";
    emit(ready,sizeof(ready)-1);
#endif
#if defined(STEAMOHOS_STACK_WRITE_OBSERVER)
    if(stack_write_initialize())return 2;
#endif
    next_action=dlsym(RTLD_NEXT,"sigaction");
    char path[4096];ssize_t length=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(length<=0||length>=(ssize_t)sizeof(path)-32)return 2;
    path[length]=0;char *slash=strrchr(path,'/');if(!slash)return 2;
    strcpy(slash+1,"UnityPlayer.so");
    /* Match the original launcher's DT_NEEDED global symbol scope. */
    void *library=dlopen(path,RTLD_NOW|RTLD_GLOBAL);
    int (*entry)(int,char **)=library?dlsym(library,"PlayerMain"):NULL;
    if(library&&!entry)entry=dlsym(library,"_Z10PlayerMainiPPc");
    if(!entry){fprintf(stderr,"GUEST_FAULT launcher_error=%s\n",dlerror());return 2;}
    fprintf(stderr,"GUEST_FAULT observer_ready=1 game_assets_modified=0\n");
    return entry(argc,argv);
}
