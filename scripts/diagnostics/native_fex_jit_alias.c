/* SPDX-License-Identifier: MIT
 * Standalone x86-64 guest JIT contract probe. No game files are changed.
 * Check first execution, repeated patching, page boundaries, and publication
 * to another thread, both with memfd RW/RX aliases and anonymous RWX memory.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct publication {
    unsigned char *rw;
    unsigned char *rx;
    atomic_uint request;
    atomic_uint complete;
    atomic_uint failed;
};
static void patch(unsigned char *rw, unsigned value) {
    rw[0] = 0xb8; /* mov eax, imm32; ret */
    memcpy(rw + 1, &value, sizeof(value));
    rw[5] = 0xc3;
    __builtin___clear_cache((char *)rw, (char *)rw + 6);
}
static unsigned execute(unsigned char *rx) {
    return ((unsigned (*)(void))(uintptr_t)rx)();
}
static void *consumer(void *raw) {
    struct publication *p = raw;
    for (unsigned i = 1; i <= 512; ++i) {
        while (atomic_load_explicit(&p->request, memory_order_acquire) != i)
            sched_yield();
        unsigned actual = execute(p->rx);
        if (actual != i)
            atomic_store_explicit(&p->failed, 1, memory_order_relaxed);
        atomic_store_explicit(&p->complete, i, memory_order_release);
    }
    return NULL;
}
static int check(int dual) {
    const char *mode = dual ? "memfd-rw-rx" : "anonymous-rwx";
    size_t page = (size_t)sysconf(_SC_PAGESIZE), bytes = page * 3;
    int fd = -1, metadata_failure = 0;
    unsigned char *rw = MAP_FAILED, *rx = MAP_FAILED;
    if (dual) {
        fd = memfd_create("native-jit-contract", MFD_CLOEXEC);
        struct stat st;
        if (fd < 0 || ftruncate(fd, (off_t)bytes)) goto error;
        int stat_result = fstat(fd, &st), stat_error = errno;
        printf("NATIVE_JIT mode=%s stage=fstat result=%d errno=%d size_matches=%d\n",
               mode, stat_result, stat_result ? stat_error : 0,
               !stat_result && st.st_size == (off_t)bytes);
        metadata_failure = stat_result != 0;
        /* Keep testing mapping/code publication even if metadata is broken;
         * report metadata failure in the final result rather than hiding it. */
        if (!stat_result && st.st_size != (off_t)bytes) goto failed;
        rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        rx = mmap(NULL, bytes, PROT_NONE, MAP_SHARED, fd, 0);
        if (rw == MAP_FAILED || rx == MAP_FAILED ||
            mprotect(rx, bytes, PROT_READ | PROT_EXEC)) goto error;
    } else {
        rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        rx = rw;
        if (rw == MAP_FAILED) goto error;
    }
    const size_t offsets[] = {64, page - 3};
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned i = 1; i <= 512; ++i) {
            patch(rw + offsets[slot], i);
            unsigned actual = execute(rx + offsets[slot]);
            if (actual != i) {
                fprintf(stderr, "NATIVE_JIT mode=%s stage=patch slot=%u expected=%u actual=%u passed=0\n",
                        mode, slot, i, actual);
                goto failed;
            }
        }
        printf("NATIVE_JIT mode=%s stage=patch slot=%u iterations=512 passed=1\n", mode, slot);
    }
    struct publication p = {.rw = rw + 64, .rx = rx + 64};
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, consumer, &p);
    if (rc) { errno = rc; goto error; }
    for (unsigned i = 1; i <= 512; ++i) {
        patch(p.rw, i);
        atomic_store_explicit(&p.request, i, memory_order_release);
        while (atomic_load_explicit(&p.complete, memory_order_acquire) != i)
            sched_yield();
    }
    rc = pthread_join(thread, NULL);
    if (rc) { errno = rc; goto error; }
    printf("NATIVE_JIT mode=%s stage=cross-thread iterations=512 passed=%u\n", mode, !atomic_load(&p.failed));
    if (atomic_load(&p.failed)) goto failed;
    if (dual) munmap(rx, bytes);
    munmap(rw, bytes);
    if (fd >= 0) close(fd);
    return metadata_failure;
error:
    fprintf(stderr, "NATIVE_JIT mode=%s stage=syscall errno=%d passed=0\n", mode, errno);
failed:
    if (dual && rx != MAP_FAILED) munmap(rx, bytes);
    if (rw != MAP_FAILED) munmap(rw, bytes);
    if (fd >= 0) close(fd);
    return 1;
}
int main(int argc, char **argv) {
    int dual = 1, rwx = 1;
    if (argc > 1 && !strcmp(argv[1], "--dual-only")) rwx = 0;
    if (argc > 1 && !strcmp(argv[1], "--rwx-only")) dual = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(45);
    puts("NATIVE_JIT observer_ready=1 bits=64");
    int failed = (dual && check(1)) | (rwx && check(0));
    printf("NATIVE_JIT_RESULT failures=%d\n", failed);
    return failed;
}
