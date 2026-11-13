#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/proton_file_mapping.h"
static void selected_library_aliases(void) {
    char temp[] = "/tmp/proton-library-XXXXXX";
    assert(mkdtemp(temp));
    const char *suffixes[] = {"/user-212", "/user-212/steamapps", "/user-212/steamapps/common",
        "/user-212/steamapps/common/中文 Game", "/user-212/steamapps/common/中文 Game-other"};
    char path[1024], selected[1024], alias[1024], sibling[1024], outside[1024];
    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(*suffixes); ++i) {
        snprintf(path, sizeof(path), "%s%s", temp, suffixes[i]);
        assert(!mkdir(path, 0700));
    }
    snprintf(path, sizeof(path), "%s/user-212", temp);
    snprintf(alias, sizeof(alias), "%s/currentUser", temp);
    assert(!symlink(path, alias));
    snprintf(selected, sizeof(selected), "%s/currentUser/steamapps/common/中文 Game", temp);
    snprintf(path, sizeof(path), "%s/user-212/steamapps/common/中文 Game/data.cpk", temp);
    snprintf(sibling, sizeof(sibling), "%s/user-212/steamapps/common/中文 Game-other/data.cpk", temp);
    snprintf(outside, sizeof(outside), "%s/user-212/unrelated.dat", temp);
    int fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
    assert(fd >= 0);
    int flags = MAP_PRIVATE | MAP_FIXED;
    assert(proton_private_read_fallback(fd, flags, path, selected));
    assert(!proton_private_read_fallback(fd, flags, sibling, selected));
    assert(!proton_private_read_fallback(fd, flags, outside, selected));
    assert(!proton_private_read_fallback(fd, MAP_SHARED | MAP_FIXED, path, selected));
    assert(!proton_private_read_fallback(fd, flags | MAP_ANONYMOUS, path, selected));
    assert(!proton_private_read_fallback(fd, flags, path, "/tmp"));
    char resolved[PATH_MAX];
    assert(!strcmp(proton_relative_to_root(path, selected, resolved), "data.cpk"));
    /* Exercise the real path returned for a file opened through a symlink. */
    char logical[1024], descriptor[64], actual[PATH_MAX];
    snprintf(logical, sizeof(logical), "%s/currentUser/steamapps/common/中文 Game/data.cpk", temp);
    int alias_fd = open(logical, O_RDONLY);
    assert(alias_fd >= 0);
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", alias_fd);
    ssize_t size = readlink(descriptor, actual, sizeof(actual) - 1);
    assert(size > 0 && size < (ssize_t)sizeof(actual) - 1);
    actual[size] = 0;
    assert(!strcmp(actual, path));
    assert(proton_private_read_fallback(alias_fd, flags, actual, selected));
    /* A file symlink escaping the game directory retains its real scope. */
    int other = open(outside, O_CREAT | O_RDWR | O_EXCL, 0600);
    assert(other >= 0 && !close(other));
    snprintf(logical, sizeof(logical), "%s/currentUser/steamapps/common/中文 Game/escape.dat", temp);
    assert(!symlink(outside, logical));
    other = open(logical, O_RDONLY);
    assert(other >= 0);
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", other);
    size = readlink(descriptor, actual, sizeof(actual) - 1);
    assert(size > 0 && size < (ssize_t)sizeof(actual) - 1);
    actual[size] = 0;
    assert(!proton_private_read_fallback(other, flags, actual, selected));
    assert(!close(other) && !unlink(logical) && !unlink(outside));
    assert(!close(alias_fd) && !close(fd) && !unlink(path) && !unlink(alias));
    for (unsigned i = sizeof(suffixes) / sizeof(*suffixes); i > 0; --i) {
        snprintf(path, sizeof(path), "%s%s", temp, suffixes[i - 1]);
        assert(!rmdir(path));
    }
    assert(!rmdir(temp));
}

int main(void) {
    char temp[] = "/tmp/proton-pe-XXXXXX";
    int fd = mkstemp(temp);
    assert(fd >= 0 && !unlink(temp));
    unsigned char image[8192] = {'M', 'Z'};
    image[60] = 128;
    memcpy(image + 128, "PE\0\0", 4);
    const char *path = "/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/drive_c/test.dll";
    int flags = MAP_PRIVATE | MAP_FIXED;
    for (unsigned machine = 0; machine < 4; ++machine) {
        unsigned types[] = {0xaa64, 0x8664, 0xa641, 0x014c};
        image[132] = types[machine] & 255; image[133] = types[machine] >> 8;
        assert(pwrite(fd, image, sizeof(image), 0) == sizeof(image));
        /* Identification reads the file header even after seeking to a
         * section. The mmap caller's section offset must not gate fallback. */
        assert(lseek(fd, 4096, SEEK_SET) == 4096);
        assert(proton_private_read_fallback(fd, flags, path, NULL));
        assert(lseek(fd, 0, SEEK_CUR) == 4096);
    }
    assert(!proton_private_read_fallback(fd, MAP_SHARED | MAP_FIXED, path, NULL));
    assert(!proton_private_read_fallback(fd, flags | MAP_ANONYMOUS, path, NULL));
    assert(!proton_private_read_fallback(fd, MAP_PRIVATE, path, NULL));
    assert(!proton_private_read_fallback(fd, flags, "/product/wine/test.dll", NULL));
    assert(!proton_private_read_fallback(fd, flags,
        "/data/app/el2/100/base/com.vintage.other/files/steamohos-native/proton-probe/test.dll", NULL));
    assert(!proton_private_read_fallback(fd, flags, "/data/storage/el2/base/files/steamohos-native/proton-probe-other/test.dll", NULL));
    const char *game = "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/NewPAL";
    const char *gamefile = "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/NewPAL/test.exe";
    assert(!proton_private_read_fallback(fd, flags, gamefile, NULL));
    assert(proton_private_read_fallback(fd, flags, gamefile, game));
    assert(!proton_private_read_fallback(fd, flags, gamefile, "/other/game"));
    const char *games[] = {"NewPAL", "Chinese Paladin 4", "SenrenBanka_trial"};
    for (unsigned i = 0; i < 3; ++i) {
        char selected[512], alias[512], sibling[1024];
        snprintf(selected, sizeof(selected), "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/%s", games[i]);
        snprintf(alias, sizeof(alias), "/storage/media/100/local/files/Docs/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/%s/game.dll", games[i]);
        assert(!proton_private_read_fallback(fd, flags, alias, selected)); /* No fabricated cross-library alias. */
        snprintf(sibling, sizeof(sibling), "%s-other/game.dll", selected);
        assert(!proton_private_read_fallback(fd, flags, sibling, selected));
        assert(!proton_private_read_fallback(fd, flags, alias, "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/Other"));
    }
    image[132] = image[133] = 0;
    assert(pwrite(fd, image, sizeof(image), 0) == sizeof(image));
    /* Data files need the same force-exec-safe private mapping as images. */
    assert(!proton_private_read_fallback(fd, flags, path, NULL));
    assert(!ftruncate(fd, 64));
    assert(!proton_private_read_fallback(fd, flags, path, NULL));
    assert(proton_private_read_fallback(fd, flags,
        "/data/storage/el2/base/files/steamohos-native/proton-probe/package/files/share/wine/nls/locale.nls", NULL));
    assert(!proton_private_read_fallback(fd, flags,
        "/data/storage/el2/base/files/steamohos-native/proton-probe/tmp/.wine-123/server-1/tmpmap-42", NULL));
    assert(!proton_private_read_fallback(fd, flags,
        "/data/storage/el2/base/files/steamohos-native/proton-probe/libc.so.6", NULL));
    assert(proton_private_read_fallback(fd, flags,
        "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/Any New Game/data.cpk",
        "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/Any New Game"));
    assert(!proton_private_read_fallback(fd, flags, gamefile,
        "/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/NewPAL/../Other"));
    const char *libraries[] = {
        "/data/storage/el2/base/files/.wine/drive_c/SteamLinuxARM64/library/steamapps/common/中文 Game",
        "/data/app/el2/212/base/com.vintage.pomelopro/files/.wine/drive_c/SteamLinuxARM64/library/steamapps/common/中文 Game",
        "/storage/media/212/local/files/Docs/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/中文 Game"
    };
    for (unsigned i = 0; i < sizeof(libraries) / sizeof(*libraries); ++i) {
        char resource[1024], escape[1024];
        snprintf(resource, sizeof(resource), "%s/data.cpk", libraries[i]);
        assert(proton_private_read_fallback(fd, flags, resource, libraries[i]));
        snprintf(escape, sizeof(escape), "%s/../Other/data.cpk", libraries[i]);
        assert(!proton_private_read_fallback(fd, flags, escape, libraries[i]));
        snprintf(escape, sizeof(escape), "%s/./data.cpk", libraries[i]);
        assert(!proton_private_read_fallback(fd, flags, escape, libraries[i]));
        snprintf(escape, sizeof(escape), "%s/.", libraries[i]);
        assert(!proton_private_read_fallback(fd, flags, resource, escape));
        snprintf(escape, sizeof(escape), "%s/..", libraries[i]);
        assert(!proton_private_read_fallback(fd, flags, resource, escape));
        assert(!proton_private_read_fallback(fd, MAP_SHARED | MAP_FIXED, resource, libraries[i]));
    }
    selected_library_aliases();
    int descriptors[2];
    assert(!pipe(descriptors));
    assert(!proton_private_read_fallback(descriptors[0], flags, path, NULL));
    close(descriptors[0]); close(descriptors[1]);
    close(fd);
    puts("PASS: private/public selected libraries, real FD aliases, non-100 users, traversal, PE/NLS and shared mapping boundaries");
}
