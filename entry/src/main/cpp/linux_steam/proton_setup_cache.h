#pragma once
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace steamohos {
enum class ProtonSetupResult { Reused, Configured, Failed, StateWriteFailed };

// Only cache successful setup in this private prefix. Failed/cancelled setup
// must be retried; a changed template or recreated registry invalidates it.
template<class Configure>
ProtonSetupResult EnsureProtonSetup(const std::string& stamp, const std::string& registry,
                                   const std::string& key, Configure configure) {
    struct stat reg{};
    const bool registryPresent = !lstat(registry.c_str(), &reg) && S_ISREG(reg.st_mode) &&
        reg.st_uid == geteuid() && reg.st_size > 0;
    int fd = open(stamp.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st{};
        std::string previous(key.size() + 1, '\0');
        const ssize_t count = read(fd, &previous[0], previous.size());
        const bool current = registryPresent && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
            st.st_uid == geteuid() && st.st_size == static_cast<off_t>(key.size()) &&
            count == static_cast<ssize_t>(key.size()) && previous.compare(0, key.size(), key) == 0;
        close(fd);
        if (current) return ProtonSetupResult::Reused;
    }
    if (!configure()) return ProtonSetupResult::Failed;
    const std::string pending = stamp + ".pending";
    fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return ProtonSetupResult::StateWriteFailed;
    struct stat st{};
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_nlink == 1;
    size_t at = 0;
    while (ok && at < key.size()) {
        const ssize_t n = write(fd, key.data() + at, key.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        at += static_cast<size_t>(n);
    }
    if (ok) ok = !fsync(fd);
    close(fd);
    if (ok) ok = !rename(pending.c_str(), stamp.c_str());
    if (!ok) unlink(pending.c_str());
    return ok ? ProtonSetupResult::Configured : ProtonSetupResult::StateWriteFailed;
}
}
