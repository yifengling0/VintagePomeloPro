// Diagnostic provenance for the real GNU Steam IPC connection. No account data.
#define _GNU_SOURCE 1
#include "steamclient_probe.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#if defined(__aarch64__)
#define NATIVE_MACHINE EM_AARCH64
#elif defined(__x86_64__)
#define NATIVE_MACHINE EM_X86_64
#else
#error Unsupported native GNU architecture
#endif

// Use the actual validated code file, bypassing writable-resource aliases.
// External execution uses anonymous RX pages on OHOS; file-backed maps are
// diagnostic only. The supervisor launches this exact manifest-validated image
// and holds its child PID alive while the kernel socket pair is checked.
static bool native_image_file(pid_t pid, const char *path, bool *mapped) {
    *mapped = false;
    int fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (fd < 0) return false;
    Elf64_Ehdr header; struct stat status;
    bool valid = pread(fd, &header, sizeof(header), 0) == sizeof(header) &&
        !syscall(SYS_fstat, fd, &status) && S_ISREG(status.st_mode) &&
        !memcmp(header.e_ident, ELFMAG, SELFMAG) && header.e_ident[EI_CLASS] == ELFCLASS64 &&
        header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_machine == NATIVE_MACHINE;
    close(fd);
    if (!valid) return false;
    char maps[80]; snprintf(maps, sizeof(maps), "/proc/%ld/maps", (long)pid);
    FILE *file = fopen(maps, "r");
    if (!file) return true;
    char line[8192], permissions[5]; unsigned major_id, minor_id; unsigned long long inode;
    valid = false;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%*x-%*x %4s %*x %x:%x %llu", permissions, &major_id, &minor_id, &inode) != 4) continue;
        if (strchr(permissions, 'x') && inode == (unsigned long long)status.st_ino &&
            major_id == major(status.st_dev) && minor_id == minor(status.st_dev)) { valid = true; break; }
    }
    fclose(file); *mapped = valid; return true;
}
struct Endpoints { struct sockaddr_storage local, peer; };
static bool tcp_endpoints(int fd, struct Endpoints *out) {
    socklen_t length = sizeof(out->local);
    if (getsockname(fd, (struct sockaddr *)&out->local, &length)) return false;
    length = sizeof(out->peer);
    if (getpeername(fd, (struct sockaddr *)&out->peer, &length)) return false;
    if (out->local.ss_family != out->peer.ss_family) return false;
    if (out->local.ss_family == AF_INET) {
        struct sockaddr_in *a = (void *)&out->local, *b = (void *)&out->peer;
        return (ntohl(a->sin_addr.s_addr) >> 24) == 127 && (ntohl(b->sin_addr.s_addr) >> 24) == 127;
    }
    if (out->local.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (void *)&out->local, *b = (void *)&out->peer;
        return IN6_IS_ADDR_LOOPBACK(&a->sin6_addr) && IN6_IS_ADDR_LOOPBACK(&b->sin6_addr);
    }
    return false;
}
static bool equal_address(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
            !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr));
    }
    return false;
}
static int descriptor(const char *name) {
    if (!*name || strspn(name, "0123456789") != strlen(name)) return -1;
    char *end; errno = 0; long value = strtol(name, &end, 10);
    return errno || *end || value < 0 || value > 1048576 ? -1 : (int)value;
}
static bool socket_inode(pid_t pid, int number, uint64_t *inode) {
    char path[96], target[96], extra; unsigned long long value;
    snprintf(path, sizeof(path), "/proc/%ld/fd/%d", (long)pid, number);
    ssize_t length = readlink(path, target, sizeof(target)-1);
    if (length < 0) return false;
    target[length] = 0;
    if (sscanf(target, "socket:[%llu]%c", &value, &extra) != 1 || !value) return false;
    *inode = value; return true;
}

bool SteamOhosVerifyClientSource(const char *library, void *factory, char *report, size_t capacity) {
    const char *expected = getenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID");
    if (!expected) return false;
    int pid = descriptor(expected);
    const char *program = getenv("STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE");
    bool image_mapped = false, module_mapped = false;
    const char *root = getenv("STEAMOHOS_CODE_ROOT"); char expected_program[4096];
    bool designated = root && snprintf(expected_program, sizeof(expected_program), "%s/steamrtarm64/steam", root) < (int)sizeof(expected_program) &&
        program && !strcmp(program, expected_program);
    bool image = pid > 0 && designated && native_image_file(pid, program, &image_mapped);
    Dl_info loaded = {0};
    bool module = dladdr(factory, &loaded) && loaded.dli_fname && !strcmp(loaded.dli_fname, library) &&
        native_image_file(getpid(), loaded.dli_fname, &module_mapped), matched = false;
    typedef int (*Borrow)(pid_t, int, uint64_t);
    Borrow borrow = (Borrow)dlsym(RTLD_DEFAULT, "SteamOhosBorrowSocket");
    typedef int (*FindPeer)(pid_t, int);
    FindPeer find_peer = (FindPeer)dlsym(RTLD_DEFAULT, "SteamOhosFindPeerSocket");
    struct Endpoints own[64]; unsigned count = 0;
    int own_fds[64];
    DIR *self = opendir("/proc/self/fd"); struct dirent *entry;
    if (self) {
        while (count < 64 && (entry = readdir(self))) {
            int fd = descriptor(entry->d_name);
            if (fd >= 0 && tcp_endpoints(fd, &own[count])) { own_fds[count] = fd; ++count; }
        }
        closedir(self);
    }
    char path[80]; snprintf(path, sizeof(path), "/proc/%d/fd", pid);
    DIR *peer = image && module && borrow ? opendir(path) : NULL;
    int directory_error = peer ? 0 : errno;
    unsigned examined = 0, borrowed = 0; int query_error = 0;
    if (image && module && find_peer) {
        for (unsigned i = 0; !matched && i < count; ++i) {
            int duplicate = find_peer(pid, own_fds[i]);
            if (duplicate < 0) { query_error = errno; continue; }
            ++borrowed; struct Endpoints remote;
            matched = tcp_endpoints(duplicate, &remote) &&
                equal_address(&own[i].local, &remote.peer) && equal_address(&own[i].peer, &remote.local);
            close(duplicate);
        }
    }
    if (peer) {
        // Older paired runtimes retain the exact PID/FD/inode protocol.
        while (!find_peer && !matched && examined++ < 4096 && (entry = readdir(peer))) {
            int fd = descriptor(entry->d_name); uint64_t inode;
            if (fd < 0 || !socket_inode(pid, fd, &inode)) continue;
            int duplicate = borrow(pid, fd, inode);
            if (duplicate < 0) continue;
            ++borrowed; struct Endpoints remote;
            if (tcp_endpoints(duplicate, &remote)) {
                for (unsigned i = 0; i < count; ++i)
                    if (equal_address(&own[i].local, &remote.peer) && equal_address(&own[i].peer, &remote.local)) {
                        matched = true; break;
                    }
            }
            close(duplicate);
        }
        closedir(peer);
    }
    fprintf(stderr, "STEAMOHOS_CLIENT_SOURCE expectedPid=%d nativeMachine=%d expectedImageNative=%d loadedLibraryNative=%d imageFileMapped=%d libraryFileMapped=%d connectedTcpSockets=%u borrowedPeerSockets=%u peerMatched=%d\n",
        pid, NATIVE_MACHINE, image, module, image_mapped, module_mapped, count, borrowed, matched);
    snprintf(report, capacity, "{\"expectedPid\":%d,\"nativeMachine\":%d,\"expectedImageNative\":%s,\"loadedLibraryNative\":%s,\"connectedTcpSockets\":%u,\"borrowedPeerSockets\":%u,\"peerMatched\":%s,\"metadataAvailable\":%s,\"peerQueryAvailable\":%s,\"peerDirectoryErrno\":%d,\"peerQueryErrno\":%d}",
        pid, NATIVE_MACHINE, image ? "true" : "false", module ? "true" : "false", count, borrowed,
        matched ? "true" : "false", borrow ? "true" : "false", find_peer ? "true" : "false", directory_error, query_error);
    return image && module && matched;
}
