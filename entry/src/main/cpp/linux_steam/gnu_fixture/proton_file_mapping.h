#ifndef HMOSDECK_PROTON_FILE_MAPPING_H
#define HMOSDECK_PROTON_FILE_MAPPING_H
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int proton_path_within(const char *path, const char *root) {
    size_t length = strlen(root);
    return !strncmp(path, root, length) && path[length] == '/';
}

/* Wine's 32-bit force-exec mode also protects data views as executable.
 * OHOS noexec file mappings cannot acquire that permission. Ask Wine to use
 * its existing anonymous+pread implementation for private file views, whether
 * they hold PE sections, NLS data or game resources. Shared views must retain
 * file visibility/writeback; never mask an actual mprotect failure. */
static int proton_private_read_fallback(int fd, int flags, const char *path, const char *game) {
    struct stat status;
    if (fd < 0 || !path || (flags & (MAP_PRIVATE | MAP_FIXED)) != (MAP_PRIVATE | MAP_FIXED) ||
        (flags & (MAP_SHARED | MAP_ANONYMOUS)) || fstat(fd, &status) || !S_ISREG(status.st_mode)) return 0;
    const char *scope = "/data/storage/el2/base/files/steamohos-native/proton-probe";
    const char *physical_scope = "/data/app/el2/100/base/com.vintage.pomelopro/files/steamohos-native/proton-probe";
    const char *relative_path = NULL;
    if (proton_path_within(path, scope)) relative_path = path + strlen(scope) + 1;
    else if (proton_path_within(path, physical_scope)) relative_path = path + strlen(physical_scope) + 1;
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
    if (!game || strstr(game, "/../") || strstr(game, "/./")) return 0;
    const char *logical = "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/";
    const char *physical = "/storage/media/100/local/files/Docs/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/";
    const char *relative;
    if (!strncmp(game, logical, strlen(logical))) relative = game + strlen(logical);
    else if (!strncmp(game, physical, strlen(physical))) relative = game + strlen(physical);
    else return 0;
    /* One selected library directory, without game-name-specific exceptions. */
    if (!*relative || !strcmp(relative, ".") || !strcmp(relative, "..") || strchr(relative, '/')) return 0;
    for (unsigned alias = 0; alias < 2; ++alias) {
        const char *root = alias ? physical : logical;
        const size_t prefix = strlen(root), length = strlen(relative);
        if (!strncmp(path, root, prefix) && !strncmp(path + prefix, relative, length) &&
            path[prefix + length] == '/') return 1;
    }
    return 0;
}
#endif
