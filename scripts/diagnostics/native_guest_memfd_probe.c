/* SPDX-License-Identifier: MIT
 * Opt-in GNU guest experiment, NOT a complete memfd implementation.
 * Compare denied memfd metadata/alias tracking against a real unlinked,
 * app-private sparse file. Never replace sealing/hugetlb requests or host
 * ARM64 JIT allocations. It is only injected into an x86-64 guest.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((constructor)) static void ready(void) {
    static const char message[]="NATIVE_JIT_FILE ready=1 scope=x86-guest experimental=1\n";
    (void)!write(2,message,sizeof(message)-1);
}

int memfd_create(const char *name, unsigned flags) {
    int (*next)(const char *, unsigned) = dlsym(RTLD_NEXT, "memfd_create");
    if (!next) { errno = ENOSYS; return -1; }
    int fd = next(name, flags), saved = errno;
    const char *enabled = getenv("STEAMOHOS_JIT_FILE_PROBE");
    if (fd < 0 || !enabled || strcmp(enabled, "1") || flags != MFD_CLOEXEC)
        return fd;
    struct stat status;
    if (!fstat(fd, &status) || (errno != EACCES && errno != EFAULT)) {
        errno = saved; return fd;
    }
    const char *root = getenv("STEAMOHOS_IPC_ROOT");
    if (!root || root[0] != '/') { errno = saved; return fd; }
    char path[PATH_MAX];
    int count = snprintf(path, sizeof(path), "%s/.jit-file-probe-XXXXXX", root);
    if (count < 0 || count >= (int)sizeof(path)) { errno = saved; return fd; }
    int replacement = mkostemp(path, O_CLOEXEC);
    if (replacement < 0) { errno = saved; return fd; }
    int unlinked = unlink(path), unlink_error = errno;
    if (unlinked || fchmod(replacement, 0700) || fstat(replacement, &status)) {
        int failure = unlinked ? unlink_error : errno;
        close(replacement); close(fd); errno = failure; return -1;
    }
    close(fd);
    dprintf(2, "NATIVE_JIT_FILE probe=1 backing=unlinked-private-file metadata=kernel flags=cloexec\n");
    errno = saved;
    return replacement;
}
