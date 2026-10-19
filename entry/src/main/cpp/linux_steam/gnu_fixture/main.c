#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern uint32_t steamohos_tls_exchange(uint32_t next);
static _Thread_local uint64_t main_tls = UINT64_C(0x1122334455667788);
static pthread_barrier_t barrier;
static atomic_int failed = 0;
static atomic_int destructors = 0;
static pthread_key_t key;
static int constructed = 0;
typedef uint32_t (*exchange_fn)(uint32_t);
static exchange_fn late_exchange;
static void __attribute__((constructor)) init(void) { constructed = 1; }
static void destroy(void* data) { if (data) atomic_fetch_add(&destructors, 1); }
static void rendezvous(void) {
    int rc = pthread_barrier_wait(&barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) atomic_store(&failed, 1);
}
static void* run(void* arg) {
    uintptr_t id = (uintptr_t)arg;
    if (main_tls != UINT64_C(0x1122334455667788) || steamohos_tls_exchange((uint32_t)id) != 17)
        atomic_store(&failed, 1);
    main_tls = id; errno = (int)id;
    if (pthread_setspecific(key, arg)) atomic_store(&failed, 1);
    rendezvous(); rendezvous();
    if (main_tls != id || errno != (int)id || steamohos_tls_exchange(17) != id)
        atomic_store(&failed, 1);
    if (!late_exchange || late_exchange((uint32_t)id) != 71 || late_exchange(71) != id)
        atomic_store(&failed, 1);
    return arg;
}
int main(int argc, char** argv) {
    if (argc != 2) return 31;
    if (!strcmp(argv[1], "exit37")) return 37;
    if (!strcmp(argv[1], "hang")) { for (;;) pause(); }
    if (strcmp(argv[1], "normal")) return 32;
    fputs("STEAMOHOS_STAGE main\n", stderr);
    if (!constructed || pthread_key_create(&key, destroy) || pthread_barrier_init(&barrier, NULL, 3)) return 33;
    pthread_t workers[2];
    fputs("STEAMOHOS_STAGE pthread_create\n", stderr);
    for (uintptr_t i = 0; i < 2; ++i)
        if (pthread_create(&workers[i], NULL, run, (void*)(i + 100))) return 34;
    fputs("STEAMOHOS_STAGE threads_running\n", stderr);
    rendezvous();
    // Resolve a real versioned GNU symbol after the two workers exist.
    void* dso = dlopen("libsteamohos_gnu_tls.so", RTLD_NOW | RTLD_LOCAL);
    exchange_fn exchange = dso ? (exchange_fn)dlvsym(dso, "steamohos_tls_exchange", "STEAMOHOS_NATIVE_1.0") : NULL;
    if (!exchange || exchange(33) != 17 || main_tls != UINT64_C(0x1122334455667788)) atomic_store(&failed, 1);
    void* late = dlopen("libsteamohos_gnu_late.so", RTLD_NOW | RTLD_LOCAL);
    late_exchange = late ? (exchange_fn)dlsym(late, "steamohos_late_exchange") : NULL;
    if (!late_exchange || late_exchange(91) != 71) atomic_store(&failed, 1);
    rendezvous();
    for (uintptr_t i = 0; i < 2; ++i) {
        void* result = NULL;
        if (pthread_join(workers[i], &result) || result != (void*)(i + 100)) atomic_store(&failed, 1);
    }
    if (exchange && exchange(17) != 33) atomic_store(&failed, 1);
    if (late_exchange && late_exchange(71) != 91) atomic_store(&failed, 1);
    if (late && dlclose(late)) atomic_store(&failed, 1);
    if (dso && dlclose(dso)) atomic_store(&failed, 1);
    if (pthread_barrier_destroy(&barrier) || pthread_key_delete(key)) atomic_store(&failed, 1);
    if (atomic_load(&failed) || atomic_load(&destructors) != 2) return 35;
    puts("STEAMOHOS_GNU_NATIVE_V1 main tls pthread errno dlvsym destructors PASS");
    return 0;
}
