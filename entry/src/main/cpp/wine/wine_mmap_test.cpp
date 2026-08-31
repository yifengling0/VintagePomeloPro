#include "wine/wine_mmap_test.h"

#include <napi/native_api.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <spawn.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

static const char* PN(int prot) {
    static char b[8];
    if (prot == 0) return "NONE";
    snprintf(b, sizeof(b), "%s%s%s",
             (prot & PROT_READ) ? "R" : "",
             (prot & PROT_WRITE) ? "W" : "",
             (prot & PROT_EXEC) ? "X" : "");
    return b;
}

static void L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void L(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OH_LOG_INFO(LOG_APP, "[MMAP] %{public}s", buf);
}

static void TestOne(const char* desc, void* p, size_t sz) {
    L("  %-20s -> %s (err=%d, %s)",
      desc, p == MAP_FAILED ? "FAIL" : "OK", errno, strerror(errno));
    if (p != MAP_FAILED) munmap(p, sz);
}

napi_value RunMmapTests(napi_env env, napi_callback_info) {
    const size_t pg = 4096;
    L("===== OHOS mmap 全量测试 (Box64/Wine 模式覆盖) =====");

    L("--- 1a. ANON|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), pg);
    }

    L("--- 1b. ANON|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "ANON|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED | MAP_ANONYMOUS, -1, 0), pg);
    }

    L("--- 1c. FILE|SHARED 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|SHAR %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_SHARED, fd, 0), pg);
        close(fd);
    }

    L("--- 1d. FILE|PRIV 4KB ---");
    for (int p = 0; p < 8; p++) {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd < 0) continue;
        ftruncate(fd, pg);
        char desc[32];
        snprintf(desc, sizeof(desc), "FILE|PRIV %s", PN(p));
        TestOne(desc, mmap(NULL, pg, p, MAP_PRIVATE, fd, 0), pg);
        close(fd);
    }

    L("--- 2. ANON|PRIV 大尺寸 RWX ---");
    size_t big[] = {0x10000, 0x100000, 0x1000000, 0x10000000};
    for (int i = 0; i < 4; i++) {
        char desc[32];
        snprintf(desc, sizeof(desc), "sz=0x%zx RWX", big[i]);
        TestOne(desc, mmap(NULL, big[i], PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), big[i]);
    }

    L("--- 3a. MAP_FIXED ANON|PRIV ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int prots[] = {PROT_READ | PROT_WRITE,
                           PROT_READ | PROT_EXEC,
                           PROT_READ | PROT_WRITE | PROT_EXEC};
            const char* names[] = {"RW", "RX", "RWX"};
            for (int i = 0; i < 3; i++) {
                char desc[64];
                snprintf(desc, sizeof(desc), "FIXED %s @%p", names[i], base);
                void* m = mmap(base, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                L("  %-24s -> %s (err=%d)", desc,
                  m == MAP_FAILED ? "FAIL" : "OK", errno);
                if (m != MAP_FAILED) munmap(m, pg * 16);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    L("--- 3b. MAP_FIXED FILE|SHARED ---");
    {
        void* base = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base != MAP_FAILED) {
            munmap(base, pg * 16);
            int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
            if (fd >= 0) {
                ftruncate(fd, pg * 16);
                int prots[] = {PROT_READ | PROT_WRITE,
                               PROT_READ | PROT_EXEC,
                               PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* names[] = {"RW", "RX", "RWX"};
                for (int i = 0; i < 3; i++) {
                    char desc[64];
                    snprintf(desc, sizeof(desc), "FIXED FILE %s @%p", names[i], base);
                    void* m = mmap(base, pg * 16, prots[i],
                                   MAP_SHARED | MAP_FIXED, fd, 0);
                    L("  %-30s -> %s (err=%d)", desc,
                      m == MAP_FAILED ? "FAIL" : "OK", errno);
                    if (m != MAP_FAILED) munmap(m, pg * 16);
                }
                close(fd);
            }
        } else {
            L("  SKIP (base mmap failed)");
        }
    }

    L("--- 4. MAP_NORESERVE ANON|PRIV ---");
    {
        int prots[] = {0, PROT_READ, PROT_READ | PROT_WRITE,
                       PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "NORESV %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 256, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                               -1, 0), pg * 256);
        }
    }

    L("--- 5. 高地址 hint RWX ---");
    {
        void* hints[] = {nullptr, (void*)0x100000000ULL, (void*)0x3f00000000ULL,
                         (void*)0x7f00000000ULL, (void*)0x7fff00000000ULL};
        const char* hnames[] = {"NULL", "4G", "0x3f00000000",
                                "0x7f00000000", "0x7fff00000000"};
        for (int i = 0; i < 5; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "hint=%s", hnames[i]);
            void* m = mmap(hints[i], pg * 16,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            L("  %-22s -> %s @%p (err=%d)", desc,
              m == MAP_FAILED ? "FAIL" : "OK", m, errno);
            if (m != MAP_FAILED) munmap(m, pg * 16);
        }
    }

    L("--- 6. MAP_32BIT (0x40) ---");
    {
        int prots[] = {0, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC,
                       PROT_READ | PROT_WRITE | PROT_EXEC};
        for (int i = 0; i < 4; i++) {
            char desc[32];
            snprintf(desc, sizeof(desc), "0x40 %s", PN(prots[i]));
            TestOne(desc, mmap(NULL, pg * 16, prots[i],
                               MAP_PRIVATE | MAP_ANONYMOUS | 0x40, -1, 0), pg * 16);
        }
    }

    L("--- 7a. mprotect ANON RW -> ... ---");
    {
        void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            for (int tp = 0; tp < 8; tp++) {
                int ret = mprotect(m, pg * 16, tp);
                L("  RW->%-3s -> %s (err=%d)", PN(tp), ret == 0 ? "OK" : "FAIL", errno);
            }
            munmap(m, pg * 16);
        } else {
            L("  SKIP");
        }
    }

    L("--- 7b. mprotect FILE SHARED RW -> RX/RWX ---");
    {
        int fd = syscall(__NR_memfd_create, "mmap_t", MFD_CLOEXEC);
        if (fd >= 0) {
            ftruncate(fd, pg * 16);
            void* m = mmap(NULL, pg * 16, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) {
                int t1[] = {PROT_READ | PROT_EXEC, PROT_READ | PROT_WRITE | PROT_EXEC};
                const char* n1[] = {"RX", "RWX"};
                for (int i = 0; i < 2; i++) {
                    int ret = mprotect(m, pg * 16, t1[i]);
                    L("  FILE RW->%-3s -> %s (err=%d)", n1[i],
                      ret == 0 ? "OK" : "FAIL", errno);
                }
                munmap(m, pg * 16);
            }
            close(fd);
        }
    }

    L("--- 8. ANON RWX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0;
            __builtin___clear_cache((char*)m, (char*)m + 4);
            L("  ANON RWX + exec -> OK");
            munmap(m, pg);
        } else {
            L("  SKIP (mmap RWX failed)");
        }
    }

    L("--- 9. Dynarec: mmap RW + mprotect RX + exec ---");
    {
        void* m = mmap(NULL, pg, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            ((uint32_t*)m)[0] = 0xD65F03C0;
            int ret = mprotect(m, pg, PROT_READ | PROT_EXEC);
            L("  RW + mprotect RX -> %s (err=%d)", ret == 0 ? "OK" : "FAIL", errno);
            if (ret == 0) {
                __builtin___clear_cache((char*)m, (char*)m + 4);
                L("  exec -> OK");
            }
            munmap(m, pg);
        }
    }

    L("--- 10. fork mmap ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            void* m = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
            printf("fork: ANON RWX -> %s (err=%d)\n", m==MAP_FAILED?"FAIL":"OK", errno);
            if (m != MAP_FAILED) { munmap(m, pg); }
            else {
                void* m2 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                if (m2 != MAP_FAILED) {
                    int r = mprotect(m2, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                    printf("fork: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                    munmap(m2, pg);
                }
            }
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    L("--- 10b. nested fork (grandchild) ---");
    {
        int fd[2]; pipe(fd);
        pid_t pid = fork();
        if (pid == 0) {
            int fd2[2]; pipe(fd2);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                close(fd2[0]); dup2(fd2[1], STDOUT_FILENO); dup2(fd2[1], STDERR_FILENO);
                setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
                printf("gc: starting mmap tests\n");
                void* gc1 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RW  -> %s (err=%d)\n", gc1==MAP_FAILED?"FAIL":"OK", errno);
                if(gc1!=MAP_FAILED) munmap(gc1, pg);
                printf("gc: about to try RWX...\n");
                void* gc2 = mmap(NULL, pg, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                printf("gc: ANON RWX -> %s (err=%d)\n", gc2==MAP_FAILED?"FAIL":"OK", errno);
                if(gc2!=MAP_FAILED) munmap(gc2, pg);
                if(gc2==MAP_FAILED) {
                    printf("gc: RWX failed, trying RW+mprotect...\n");
                    void* gc3 = mmap(NULL, pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                    if(gc3!=MAP_FAILED){
                        int r=mprotect(gc3, pg, PROT_READ|PROT_WRITE|PROT_EXEC);
                        printf("gc: RW+mprotect->RWX -> %s (err=%d)\n", r==0?"OK":"FAIL", errno);
                        munmap(gc3, pg);
                    }
                }
                printf("gc: done\n");
                close(fd2[1]); _exit(0);
            }
            close(fd2[1]);
            char buf2[512]={0}; read(fd2[0], buf2, sizeof(buf2)-1); close(fd2[0]);
            int status;
            waitpid(pid2, &status, 0);
            close(fd[0]); dup2(fd[1], STDOUT_FILENO); dup2(fd[1], STDERR_FILENO);
            printf("%s", buf2);
            if(WIFSIGNALED(status)) printf("gc_child: KILLED by signal %d\n", WTERMSIG(status));
            else if(WIFEXITED(status)) printf("gc_child: exited=%d\n", WEXITSTATUS(status));
            close(fd[1]); _exit(0);
        }
        close(fd[1]);
        char buf[512]={0}; read(fd[0], buf, sizeof(buf)-1); close(fd[0]);
        for (char* p=strtok(buf,"\n"); p; p=strtok(NULL,"\n")) L("  %s", p);
        waitpid(pid, NULL, 0);
    }

    L("--- 11. posix_spawn mmap_test ---");
    {
        int fd[2]; pipe(fd);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDERR_FILENO);
        pid_t pid;
        const char* spawn_bin = "/data/storage/el2/base/files/wine/bin/mmap_test";
        char* argv[] = { (char*)spawn_bin, NULL };
        extern char** environ;
        int ret = posix_spawn(&pid, spawn_bin, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fd[1]);
        if (ret == 0) {
            char buf[4096]={0}; ssize_t n = read(fd[0], buf, sizeof(buf)-1);
            if (n > 0) {
                char* save; char* line = strtok_r(buf, "\n", &save);
                while (line) {
                    if (strstr(line, "RWX") || strstr(line, "mprotect") || strstr(line, "FAIL")) L("  %s", line);
                    line = strtok_r(NULL, "\n", &save);
                }
            }
            waitpid(pid, NULL, 0);
        } else {
            L("  FAILED err=%d(%s)", ret, strerror(ret));
        }
        close(fd[0]);
    }

    L("===== 测试完成 =====");
    return nullptr;
}
