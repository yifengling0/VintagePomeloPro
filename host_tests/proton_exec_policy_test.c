#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
static _Thread_local int calls, reject;
static int policy_prctl(int option, ...) {
    assert(option == 0x6a6974);
    ++calls;
    errno = EPERM;
    return reject ? -1 : 0;
}
#define prctl policy_prctl
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/proton_exec_policy.h"
#undef prctl
static void *thread_main(void *unused) {
    (void)unused;
    assert(!calls);
    errno = EBUSY;
    reject = 1;
    proton_prepare_exec_policy(PROT_EXEC);
    proton_prepare_exec_policy(PROT_EXEC);
    assert(calls == 2 && errno == EBUSY); /* Failed policy requests retry. */
    reject = 0;
    proton_prepare_exec_policy(PROT_EXEC);
    proton_prepare_exec_policy(PROT_EXEC);
    assert(calls == 4 && errno == EBUSY);
    return NULL;
}
int main(void) {
    unsetenv("STEAMOHOS_CODE_ROOT");
    proton_prepare_exec_policy(PROT_EXEC);
    setenv("STEAMOHOS_CODE_ROOT", "/product/runtime", 1);
    proton_prepare_exec_policy(PROT_EXEC);
    assert(!calls);
    setenv("STEAMOHOS_CODE_ROOT", "/data/storage/el2/base/files/steamohos-native/proton-probe", 1);
    proton_prepare_exec_policy(PROT_READ);
    assert(!calls);
    errno = EBUSY;
    proton_prepare_exec_policy(PROT_EXEC);
    proton_prepare_exec_policy(PROT_EXEC);
    assert(calls == 2 && errno == EBUSY);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, thread_main, NULL));
    assert(!pthread_join(thread, NULL));
    assert(calls == 2);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        proton_prepare_exec_policy(PROT_EXEC);
        proton_prepare_exec_policy(PROT_EXEC);
        _exit(calls == 4 ? 0 : 1);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* Preparing policy cannot turn a real failed mprotect into success. */
    proton_prepare_exec_policy(PROT_EXEC);
    assert(mprotect((void *)1, 4096, PROT_READ | PROT_EXEC) == -1 && errno == EINVAL);
    puts("PASS: experiment scope, thread/fork policy, repeated transitions, errno and real protection failure");
}
