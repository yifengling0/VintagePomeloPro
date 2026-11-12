#ifndef HMOSDECK_PROTON_FILE_MAPPING_H
#define HMOSDECK_PROTON_FILE_MAPPING_H
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Paths come from the selected Steam installation and /proc/self/fd.
 * Reject lexical traversal before checking either spelling of a directory. */
static int proton_clean_absolute_path(const char *path) {
    if (!path || path[0] != '/' || !path[1]) return 0;
    for (const char *part = path + 1; *part;) {
        const char *end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1 && part[0] == '.') ||
            (length == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) return 1;
        part = end + 1;
        if (!*part) return 0;
    }
    return 0;
}

static int proton_path_within(const char *path, const char *root) {
    size_t length = strlen(root);
    return length && !strncmp(path, root, length) && path[length] == '/';
}

static const char *proton_relative_to_root(const char *path, const char *root,
                                          char resolved[PATH_MAX]) {
    if (proton_path_within(path, root)) return path + strlen(root) + 1;
    /* The kernel reports the real FD path, while Steam may retain a sandbox
     * or picker alias. Resolve that selected root; never guess a user ID or
     * accept the matching game name from an unrelated library. */
    if (realpath(root, resolved) && proton_path_within(path, resolved))
        return path + strlen(resolved) + 1;
    return NULL;
}

static int proton_selected_game_root(const char *game) {
    if (!proton_clean_absolute_path(game)) return 0;
    const char *name = strrchr(game, '/');
    const char parent[] = "/steamapps/common";
    size_t length = (size_t)(name - game);
    return length >= sizeof(parent) - 1 &&
        !memcmp(name - (sizeof(parent) - 1), parent, sizeof(parent) - 1);
}

/* Wine's 32-bit force-exec mode also protects data views as executable.
 * OHOS noexec file mappings cannot acquire that permission. Ask Wine to use
 * its existing anonymous+pread implementation for private file views, whether
 * they hold PE sections, NLS data or game resources. Shared views must retain
 * file visibility/writeback; never mask an actual mprotect failure. */
static int proton_private_read_fallback(int fd, int flags, const char *path, const char *game) {
    struct stat status;
    if (fd < 0 || !proton_clean_absolute_path(path) || (flags & (MAP_PRIVATE | MAP_FIXED)) != (MAP_PRIVATE | MAP_FIXED) ||
        (flags & (MAP_SHARED | MAP_ANONYMOUS)) || fstat(fd, &status) || !S_ISREG(status.st_mode)) return 0;
    const char *scope = "/data/storage/el2/base/files/steamohos-native/proton-probe";
    /* The broker has already validated the selected installation against its
     * active Steam library. The direct match is the usual, allocation-free
     * path, including the default private C: library. */
    const int selected = proton_selected_game_root(game);
    if (selected && proton_path_within(path, game)) return 1;
    char resolved[PATH_MAX];
    const char *relative_path = proton_relative_to_root(path, scope, resolved);
    if (relative_path) {
        // Wine session/user shared data lives in tmp/server-* and relies on
        // seeing server updates even through read-only MAP_PRIVATE views.
        // Keep those mappings and the GNU ELF loader entirely unchanged.
        if (!strncmp(relative_path, "package/files/share/wine/nls/", 29)) return 1;
        unsigned char dos[64], pe[6];
        if (pread(fd, dos, sizeof(dos), 0) != sizeof(dos) || dos[0] != 'M' || dos[1] != 'Z') return 0;
        uint32_t position = (uint32_t)dos[60] | ((uint32_t)dos[61] << 8) |
            ((uint32_t)dos[62] << 16) | ((uint32_t)dos[63] << 24);
        if (position >= 1024 * 1024 || pread(fd, pe, sizeof(pe), position) != sizeof(pe) || memcmp(pe, "PE\0\0", 4)) return 0;
        unsigned machine = pe[4] | ((unsigned)pe[5] << 8);
        return machine == 0xaa64 || machine == 0x8664 || machine == 0xa641 || machine == 0x014c;
    }
    return selected && proton_relative_to_root(path, game, resolved) != NULL;
}
#endif
