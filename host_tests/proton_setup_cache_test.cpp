#include "../entry/src/main/cpp/linux_steam/proton_setup_cache.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main() {
    char path[] = "/tmp/proton-setup-XXXXXX";
    const auto dir = mkdtemp(path); assert(dir);
    const std::string root(dir), stamp = root + "/setup.state", registry = root + "/user.reg";
    using steamohos::EnsureProtonSetup;
    using R = steamohos::ProtonSetupResult;
    int calls = 0;
    auto configure = [&]() { ++calls; std::ofstream(registry) << "WINE REGISTRY Version 2\n"; return true; };
    assert(EnsureProtonSetup(stamp, registry, "config-v1", configure) == R::Configured);
    assert(EnsureProtonSetup(stamp, registry, "config-v1", configure) == R::Reused && calls == 1);
    assert(EnsureProtonSetup(stamp, registry, "config-v2", configure) == R::Configured && calls == 2);
    auto fail = [&]() { ++calls; return false; };
    assert(EnsureProtonSetup(stamp, registry, "config-v3", fail) == R::Failed);
    assert(EnsureProtonSetup(stamp, registry, "config-v3", configure) == R::Configured && calls == 4);
    unlink(registry.c_str());
    assert(EnsureProtonSetup(stamp, registry, "config-v3", configure) == R::Configured && calls == 5);
    std::ofstream(stamp) << "truncated";
    assert(EnsureProtonSetup(stamp, registry, "config-v3", configure) == R::Configured && calls == 6);
    unlink(stamp.c_str()); assert(!symlink(registry.c_str(), stamp.c_str()));
    assert(EnsureProtonSetup(stamp, registry, "config-v3", configure) == R::Configured && calls == 7);
    assert(!symlink(registry.c_str(), (stamp + ".pending").c_str()));
    assert(EnsureProtonSetup(stamp, registry, "config-v4", configure) == R::StateWriteFailed);
    unlink((stamp + ".pending").c_str());
    assert(EnsureProtonSetup(stamp, registry, "config-v4", configure) == R::Configured);
    unlink(stamp.c_str()); unlink(registry.c_str()); rmdir(root.c_str());
    std::cout << "PASS: setup reused, changed template, failed/cancelled retry, recreated registry, truncated/symlink stamp and interrupted commit\n";
}
