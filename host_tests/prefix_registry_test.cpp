#include "wine/prefix_registry.h"
#include "wine/prefix_registry_io.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>

static std::string shellView(bool wow64, bool emptyServer = false)
{
    const std::string base = wow64 ? R"(Software\\Classes\\Wow6432Node\\CLSID\\)"
                                  : R"(Software\\Classes\\CLSID\\)";
    std::string result;
    for (const char* clsid : {"{71F96385-DDD6-48D3-A0C1-AE06E8B055FB}",
                             "{9BA05972-F6A8-11CF-A442-00A0C90A8F39}"}) {
        result += "[" + base + clsid + R"(\\InprocServer32] 1790834916)" "\n";
        result += emptyServer ? "@=\"\"\n" : "@=\"C:\\\\windows\\\\system32\\\\shell32.dll\"\n";
    }
    return result;
}

int main(int argc, char** argv)
{
    char path[] = "/tmp/wine-prefix-registry-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    auto write = [&](const std::string& data) {
        FILE* file = fopen(path, "w");
        assert(file);
        assert(fwrite(data.data(), 1, data.size(), file) == data.size());
        assert(fclose(file) == 0);
    };
    // The broken device prefix was nonempty, with an update timestamp and
    // filesystem, but lacked the COM registrations that Explorer needs.
    write("WINE REGISTRY Version 2\n[Software\\\\Wine] 1790834550\n\"Version\"=\"10\"\n");
    assert(!winehua::HasPrefixShellRegistrations(path));
    write(shellView(false));
    assert(!winehua::HasPrefixShellRegistrations(path));
    write(shellView(true));
    assert(!winehua::HasPrefixShellRegistrations(path));
    write(shellView(false) + shellView(true, true));
    assert(!winehua::HasPrefixShellRegistrations(path));
    write(shellView(false) + shellView(true));
    assert(winehua::HasPrefixShellRegistrations(path));
    // A loader holding the original inode must see the entire old hive even
    // when a migration publishes its replacement (the original O_TRUNC bug).
    const std::string original = "WINE REGISTRY Version 2\n" + shellView(false) +
        std::string(4 * 1024 * 1024, '\n') + shellView(true);
    write(original);
    int reader = open(path, O_RDONLY);
    assert(reader >= 0);
    std::string loaded;
    assert(winehua::ReadPrefixRegistry(path, loaded) && loaded == original);
    const std::string migrated = original + "\n[System] 0\n\"ACP\"=\"936\"\n";
    assert(winehua::WritePrefixRegistry(path, migrated));
    char buf[65536];
    std::string held;
    ssize_t n;
    while ((n = read(reader, buf, sizeof(buf))) > 0) held.append(buf, n);
    assert(n == 0 && held == original);
    close(reader);
    assert(winehua::ReadPrefixRegistry(path, loaded) && loaded == migrated);
    assert(winehua::HasPrefixShellRegistrations(path));
    write("partial registry without header");
    assert(!winehua::ReadPrefixRegistry(path, loaded) && loaded.empty());
    assert(!winehua::ReadPrefixRegistry("/tmp", loaded) && loaded.empty());
    unlink(path);
    assert(!winehua::HasPrefixShellRegistrations(path));
    // Optional real pre-fix and WineHua registry captures, kept outside Git.
    if (argc == 3) {
        assert(!winehua::HasPrefixShellRegistrations(argv[1]));
        assert(winehua::HasPrefixShellRegistrations(argv[2]));
    }
    puts("prefix registry regression checks passed");
}
