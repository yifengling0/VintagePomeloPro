/* SPDX-License-Identifier: MIT
 * Invoke an inspected VP8 block predictor, including its own stack scratch and
 * horizontal/vertical SSSE3 helpers. Diagnostic only: never patch the library.
 * Supply offsets and a matching ELF identity in the external test manifest.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <time.h>
#include <ucontext.h>
#ifndef PROBE_LIBRARY
#error Supply PROBE_LIBRARY and validated ELF offsets for this private diagnostic
#endif
#ifndef PROBE_BLOCK_SIZE
#define PROBE_BLOCK_SIZE 16
#endif
#if PROBE_BLOCK_SIZE != 8 && PROBE_BLOCK_SIZE != 16
#error Requires an inspected 8x8 or 16x16 predictor entry
#endif
typedef void (*Predictor)(const unsigned char *,int,int,int,unsigned char *,int);
static const int filters[8][6]={
    {0,0,128,0,0,0},{0,-6,123,12,-1,0},{2,-11,108,36,-8,1},
    {0,-9,93,50,-6,0},{3,-16,77,77,-16,3},{0,-6,50,93,-9,0},
    {1,-8,36,108,-11,2},{0,-1,12,123,-6,0}
};
enum {MAX_STRIDE=1984,SOURCE_BYTES=MAX_STRIDE*32+64,DEST_BYTES=MAX_STRIDE*16+128};
static Predictor predictor;
static atomic_uint failures,cases;
#ifndef PROBE_SIGNAL_STRESS
#define PROBE_SIGNAL_STRESS 0
#endif
#ifndef PROBE_SIGNAL_SAME_STACK
#define PROBE_SIGNAL_SAME_STACK 0
#endif
static atomic_uint delivered_signals;
#if PROBE_SIGNAL_STRESS
static void emit(const void *data,size_t bytes){ssize_t rc=write(2,data,bytes);(void)rc;}
static char *hex(char *out,unsigned long value){static const char digits[]="0123456789abcdef";char reverse[16];unsigned n=0;do{reverse[n++]=digits[value&15];value>>=4;}while(value);while(n)*out++=reverse[--n];return out;}
#include "native_guest_signal_frames.h"
#endif
#if PROBE_SIGNAL_STRESS
static void asynchronous_signal(int sig,siginfo_t *info,void *raw) {
    (void)info;signal_frame_record(sig,raw);
    atomic_fetch_add_explicit(&delivered_signals,1,memory_order_relaxed);
}
#endif
static int clip(int v){return v<0?0:v>255?255:v;}
static void fault(int sig,siginfo_t *info,void *raw) {
    const ucontext_t *u=raw;
    dprintf(2,"VP8_PREDICTOR fault signal=%d address=%p rip=%llx rsp=%llx\n",sig,info->si_addr,
        (unsigned long long)u->uc_mcontext.gregs[REG_RIP],(unsigned long long)u->uc_mcontext.gregs[REG_RSP]);
    static const int indices[]={REG_RAX,REG_RBX,REG_RCX,REG_RDX,REG_RSI,REG_RDI,REG_RBP,REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
    static const char *names[]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15"};
    for(unsigned i=0;i<sizeof(indices)/sizeof(indices[0]);i++)dprintf(2,"VP8_PREDICTOR register %s=%llx\n",names[i],(unsigned long long)u->uc_mcontext.gregs[indices[i]]);
#if PROBE_SIGNAL_STRESS
    signal_frame_record(sig,u);signal_frame_dump();
    unsigned char bytes[256];uintptr_t start=u->uc_mcontext.gregs[REG_RSP];
    long n=signal_read(start,bytes,sizeof(bytes));
    if(n>0){char line[600],*out=stpcpy(line,"VP8_PREDICTOR fault_stack bytes=");for(long i=0;i<n;i++){static const char digits[]="0123456789abcdef";*out++=digits[bytes[i]>>4];*out++=digits[bytes[i]&15];}*out++='\n';emit(line,(size_t)(out-line));}
    int fd=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);
    if(fd>=0){char page[4096],line[4096];size_t used=0,total=0;ssize_t count;
        while(total<4*1024*1024&&(count=read(fd,page,sizeof(page)))>0){total+=(size_t)count;
            for(ssize_t i=0;i<count;i++){if(page[i]!='\n'){if(used<sizeof(line)-2)line[used++]=page[i];continue;}
                line[used]=0;unsigned long low=0,high=0;
                if(sscanf(line,"%lx-%lx",&low,&high)==2&&(strstr(line,"UnityPlayer.so")||
                    (low<=(uintptr_t)u->uc_mcontext.gregs[REG_RIP]&&(uintptr_t)u->uc_mcontext.gregs[REG_RIP]<high))){dprintf(2,"VP8_PREDICTOR map %s\n",line);}used=0;}}
        close(fd);}
#endif
    _exit(128+sig);
}
static unsigned scalar(const unsigned char *src,int stride,unsigned x,unsigned y,unsigned xp,unsigned yp) {
    int sum=0;
    for(unsigned j=0;j<6;j++) {
        int row=0;
        for(unsigned i=0;i<6;i++)row+=src[((int)y+(int)j-2)*stride+(int)x+(int)i-2]*filters[xp][i];
        sum+=clip((row+64)>>7)*filters[yp][j];
    }
    return (unsigned)clip((sum+64)>>7);
}
static void *worker(void *raw) {
    uintptr_t job=(uintptr_t)raw;
    unsigned char *input=aligned_alloc(32,(SOURCE_BYTES+31u)&~31u);
    unsigned char *heap=aligned_alloc(32,(DEST_BYTES+31u)&~31u);
    unsigned char *expected=malloc(DEST_BYTES);
    unsigned char stack[DEST_BYTES] __attribute__((aligned(32)));
    if(!input||!heap||!expected){atomic_fetch_add(&failures,1);goto done;}
#if PROBE_SIGNAL_STRESS
    unsigned char *alternate=malloc(65536);
    stack_t previous,signal_stack={.ss_sp=alternate,.ss_size=65536};
    timer_t timer;
    if(!alternate||sigaltstack(PROBE_SIGNAL_SAME_STACK?&(stack_t){.ss_flags=SS_DISABLE}:&signal_stack,&previous)){
        fprintf(stderr,"VP8_PREDICTOR signal_setup_error errno=%d\n",errno);
        free(alternate);atomic_fetch_add(&failures,1);goto done;
    }
    struct sigevent event={.sigev_notify=SIGEV_THREAD_ID,.sigev_signo=SIGUSR1};
    event._sigev_un._tid=(pid_t)syscall(SYS_gettid);
    if(timer_create(CLOCK_MONOTONIC,&event,&timer)){
        fprintf(stderr,"VP8_PREDICTOR timer_setup_error errno=%d\n",errno);
        sigaltstack(&previous,NULL);free(alternate);atomic_fetch_add(&failures,1);goto done;
    }
    struct itimerspec interval={.it_interval={.tv_nsec=100000},.it_value={.tv_nsec=100000}};
    if(timer_settime(timer,0,&interval,NULL)){
        fprintf(stderr,"VP8_PREDICTOR timer_start_error errno=%d\n",errno);
        timer_delete(timer);sigaltstack(&previous,NULL);free(alternate);
        atomic_fetch_add(&failures,1);goto done;
    }
#endif
    const int strides[]={32,64,1984};
    for(unsigned seed=0;seed<(PROBE_SIGNAL_STRESS?32u:4u);seed++) {
        for(unsigned i=0;i<SOURCE_BYTES;i++)input[i]=(unsigned char)((i*13u+(i>>3)*31u+seed*71u+job*29u)^(i>>7));
        for(unsigned si=0;si<3;si++)for(unsigned xp=0;xp<8;xp++)for(unsigned yp=0;yp<8;yp++)
        for(unsigned alignment=0;alignment<4;alignment++)for(unsigned on_stack=0;on_stack<2;on_stack++) {
            int stride=strides[si];unsigned source_offset=2u*(unsigned)stride+2u+alignment;
            unsigned char *out=on_stack?stack:heap;
            memset(out,0xa5,DEST_BYTES);memset(expected,0xa5,DEST_BYTES);
            const unsigned char *src=input+source_offset;
            for(unsigned y=0;y<PROBE_BLOCK_SIZE;y++)for(unsigned x=0;x<PROBE_BLOCK_SIZE;x++)
                expected[64+y*(unsigned)stride+x]=(unsigned char)scalar(src,stride,x,y,xp,yp);
            predictor(src,stride,(int)xp,(int)yp,out+64,stride);
            if(memcmp(out,expected,DEST_BYTES)) {
                unsigned at=0;while(at<DEST_BYTES&&out[at]==expected[at])at++;
                fprintf(stderr,"VP8_PREDICTOR mismatch job=%lu stride=%d xp=%u yp=%u alignment=%u stack=%u at=%u got=%u expected=%u\n",
                    (unsigned long)job,stride,xp,yp,alignment,on_stack,at,out[at],expected[at]);
                atomic_fetch_add(&failures,1);goto cleanup_signal;
            }
            atomic_fetch_add(&cases,1);
        }
    }
cleanup_signal:
#if PROBE_SIGNAL_STRESS
    /* Stop delivery before the alternate signal stack is released. */
    sigset_t blocked,old_mask;sigemptyset(&blocked);sigaddset(&blocked,SIGUSR1);
    pthread_sigmask(SIG_BLOCK,&blocked,&old_mask);
    timer_delete(timer);
    struct timespec no_wait={0,0};while(sigtimedwait(&blocked,NULL,&no_wait)>=0){}
    sigaltstack(&previous,NULL);free(alternate);
    pthread_sigmask(SIG_SETMASK,&old_mask,NULL);
#endif
done:
    free(input);free(heap);free(expected);return NULL;
}
int main(int argc,char **argv) {
    alarm(120);struct sigaction sa={.sa_flags=SA_SIGINFO,.sa_sigaction=fault};
    sigemptyset(&sa.sa_mask);sigaction(SIGSEGV,&sa,NULL);sigaction(SIGILL,&sa,NULL);
    const char *library=argc==2&&argv[1][0]=='/'?argv[1]:PROBE_LIBRARY;
    void *h=dlopen(library,RTLD_NOW|RTLD_LOCAL);struct link_map *map=NULL;
    if(!h||dlinfo(h,RTLD_DI_LINKMAP,&map)||!map){fprintf(stderr,"VP8_PREDICTOR load_failed %s\n",dlerror());return 2;}
    const char *version=*(const char **)(map->l_addr+PROBE_VERSION_OFFSET);
    if(!version||strncmp(version,"WebM Project VP8 Decoder v1.3.0",30))return 2;
    predictor=(Predictor)(map->l_addr+PROBE_PREDICTOR_OFFSET);
    fprintf(stderr,"VP8_PREDICTOR begin threads=4 phases=64 block=%dx%d gpu_calls=0\n",PROBE_BLOCK_SIZE,PROBE_BLOCK_SIZE);
#if PROBE_SIGNAL_STRESS
    struct sigaction async={.sa_flags=SA_SIGINFO|(PROBE_SIGNAL_SAME_STACK?0:SA_ONSTACK)|SA_RESTART,.sa_sigaction=asynchronous_signal};
    fprintf(stderr,"VP8_PREDICTOR signal_stack=%s period_us=100\n",PROBE_SIGNAL_SAME_STACK?"current":"alternate");
    sigemptyset(&async.sa_mask);
    if(sigaction(SIGUSR1,&async,NULL))return 2;
#endif
    pthread_t threads[4];unsigned created=0;
    for(unsigned i=0;i<4;i++){if(pthread_create(&threads[i],NULL,worker,(void *)(uintptr_t)i)){atomic_fetch_add(&failures,1);break;}created++;}
    for(unsigned i=0;i<created;i++)pthread_join(threads[i],NULL);
    unsigned failed=atomic_load(&failures),count=atomic_load(&cases);
    if(PROBE_SIGNAL_STRESS&&atomic_load(&delivered_signals)<64)failed++;
    fprintf(stderr,"VP8_PREDICTOR complete cases=%u guard_mismatches=%u result=%u signals=%u\n",count,failed,!!failed,atomic_load(&delivered_signals));
    dlclose(h);return failed?1:0;
}
