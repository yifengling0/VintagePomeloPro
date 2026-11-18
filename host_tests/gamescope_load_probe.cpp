// SPDX-License-Identifier: LGPL-2.1-or-later
// Device loader check only. This utility is never included in the HAP.
#include <cstdio>
#include <dlfcn.h>

int main() {
    void* library = dlopen("libgamescope_core.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "GAMESCOPE_LOAD_FAILED: %s\n", dlerror()); return 1; }
    auto entry = reinterpret_cast<int(*)(int, char**)>(dlsym(library, "main"));
    if (!entry) { std::fprintf(stderr, "GAMESCOPE_ENTRY_FAILED: %s\n", dlerror()); return 2; }
    char name[] = "gamescope";
    char version[] = "--version";
    char* argv[]{name, version, nullptr};
    // Upstream --version exits before creating a backend or display server.
    return entry(2, argv);
}
