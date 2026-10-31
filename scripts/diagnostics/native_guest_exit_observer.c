/* SPDX-License-Identifier: MIT
 * Opt-in guest LD_PRELOAD diagnostic. Observe explicit termination calls;
 * preserve their status and behavior. No signal hooks, credentials or memory
 * payloads are captured. Absence of a trace does not exclude a direct syscall.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static atomic_flag reporting = ATOMIC_FLAG_INIT;
static int (*observed_main)(int,char**,char**);
static void report(const char *api, int status) {
    if (atomic_flag_test_and_set(&reporting)) return;
    void *frames[24];
    int count = backtrace(frames, 24);
    dprintf(2, "NATIVE_EXIT api=%s status=%d frames=%d\n", api, status, count);
    for (int i = 1; i < count; ++i) {
        Dl_info info = {0};
        if (!dladdr(frames[i], &info)) {
            dprintf(2, "NATIVE_EXIT frame=%d module=unknown pc=%p\n", i, frames[i]);
            continue;
        }
        const char *name = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        name = name ? name + 1 : (info.dli_fname ? info.dli_fname : "unknown");
        dprintf(2, "NATIVE_EXIT frame=%d module=%s offset=0x%lx symbol=%.160s\n", i, name,
                (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase),
                info.dli_sname ? info.dli_sname : "unknown");
    }
}
__attribute__((constructor)) static void ready(void) {
    static const char line[] = "NATIVE_EXIT observer_ready=1 scope=guest termination_changed=0\n";
    (void)!write(2, line, sizeof(line) - 1);
}
static int main_return(int argc,char **argv,char **envp) {
    int result=observed_main(argc,argv,envp);
    report("main-return",result);
    return result;
}
int __libc_start_main(int (*entry)(int,char**,char**),int argc,char **argv,
                      void (*init)(void),void (*fini)(void),
                      void (*rtld_fini)(void),void *stack_end) {
    typedef int (*start_fn)(int (*)(int,char**,char**),int,char**,
                           void (*)(void),void (*)(void),void (*)(void),void*);
    start_fn next=(start_fn)dlsym(RTLD_NEXT,"__libc_start_main");
    if(!next) {syscall(SYS_exit_group,127);__builtin_unreachable();}
    observed_main=entry;
    return next(main_return,argc,argv,init,fini,rtld_fini,stack_end);
}
__attribute__((visibility("hidden"))) void SteamOhosObserveGuestExit(long number,long status) {
    report(number == SYS_exit_group ? "syscall-exit-group" : "syscall-exit", (int)status);
}
__attribute__((noreturn)) void exit(int status) {
    report("exit", status);
    void (*next)(int) = dlsym(RTLD_NEXT, "exit");
    if (next) next(status);
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
__attribute__((noreturn)) void _exit(int status) {
    report("_exit", status);
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}
__attribute__((noreturn)) void _Exit(int status) {
    _exit(status);
}
__attribute__((noreturn)) void abort(void) {
    report("abort", 134);
    void (*next)(void) = dlsym(RTLD_NEXT, "abort");
    if (next) next();
    syscall(SYS_exit_group, 134);
    __builtin_unreachable();
}
