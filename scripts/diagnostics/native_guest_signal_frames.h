/* SPDX-License-Identifier: MIT
 * Opt-in, bounded history of guest signal frame locations. Read-only observer;
 * no FEX, handler, stack, instruction, or saved-register modifications.
 */
#include <stdatomic.h>
enum {SIGNAL_HISTORY=64,SIGNAL_FIELDS=11};
static _Atomic unsigned long signal_sequence;
static _Atomic unsigned long signal_history[SIGNAL_HISTORY][SIGNAL_FIELDS];
static long signal_read(uintptr_t address,void *out,size_t size) {
    struct iovec local={out,size},remote={(void *)address,size};
    return syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0);
}
static void signal_frame_record(int sig,const ucontext_t *u) {
    unsigned long serial=atomic_fetch_add_explicit(&signal_sequence,1,memory_order_relaxed)+1;
    unsigned slot=(unsigned)((serial-1)%SIGNAL_HISTORY);
    uintptr_t fp=(uintptr_t)u->uc_mcontext.fpregs,fp_end=0;
    unsigned int sw[5]={0},tail=0;
    if(fp<=UINTPTR_MAX-16384&&signal_read(fp+464,sw,sizeof(sw))==(long)sizeof(sw)&&
       sw[0]==0x46505853&&sw[1]>=512&&sw[1]<=16384) {
        fp_end=fp+sw[1];
        signal_read(fp_end-4,&tail,sizeof(tail));
    }
    uintptr_t sp=(uintptr_t)u->uc_mcontext.gregs[REG_RSP];
    unsigned overlap=fp_end&&sp>=128&&fp_end>sp-128&&
        ((fp<=sp&&sp-fp<65536)||(fp>sp&&fp-sp<65536));
    unsigned long values[SIGNAL_FIELDS]={serial,(unsigned long)syscall(SYS_gettid),(unsigned long)sig,
        (uintptr_t)u->uc_mcontext.gregs[REG_RIP],sp,(uintptr_t)u,fp,fp_end,tail,overlap,(uintptr_t)u->uc_stack.ss_sp};
    atomic_store_explicit(&signal_history[slot][0],0,memory_order_release);
    for(unsigned i=1;i<SIGNAL_FIELDS;i++)atomic_store_explicit(&signal_history[slot][i],values[i],memory_order_relaxed);
    atomic_store_explicit(&signal_history[slot][0],serial,memory_order_release);
}
static void signal_frame_dump(void) {
    unsigned long end=atomic_load_explicit(&signal_sequence,memory_order_acquire);
    unsigned long start=end>SIGNAL_HISTORY?end-SIGNAL_HISTORY+1:1;
    static const char *names[SIGNAL_FIELDS]={"sequence","tid","signal","rip","rsp","context","fp","fp_end","tail","near_sp_overlap","registered_alt_sp"};
    for(unsigned long serial=start;serial<=end;serial++) {
        unsigned slot=(unsigned)((serial-1)%SIGNAL_HISTORY);
        if(atomic_load_explicit(&signal_history[slot][0],memory_order_acquire)!=serial)continue;
        unsigned long data[SIGNAL_FIELDS];data[0]=serial;
        for(unsigned i=1;i<SIGNAL_FIELDS;i++)data[i]=atomic_load_explicit(&signal_history[slot][i],memory_order_relaxed);
        if(atomic_load_explicit(&signal_history[slot][0],memory_order_acquire)!=serial)continue;
        char line[1024],*out=stpcpy(line,"GUEST_SIGNAL_FRAME");
        for(unsigned i=0;i<SIGNAL_FIELDS;i++){*out++=' ';out=stpcpy(out,names[i]);*out++='=';out=hex(out,data[i]);}
        *out++='\n';emit(line,(size_t)(out-line));
    }
}
