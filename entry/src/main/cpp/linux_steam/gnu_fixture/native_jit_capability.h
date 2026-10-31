// SPDX-License-Identifier: MIT
// Capability fallback for native x86 CoreCLR under the OHOS FEX launcher.
// No replacement memfd, invented inode, or change to host/GPU mappings.
#ifndef STEAMOHOS_NATIVE_JIT_CAPABILITY_H
#define STEAMOHOS_NATIVE_JIT_CAPABILITY_H
#include <cerrno>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace steamohos::fex {
enum class NativeJitPolicy { Unchanged, MetadataDenied, EnvironmentError };
inline NativeJitPolicy ApplyNativeJitCapability() {
    // Both prefixes are supported by CoreCLR. An explicit user choice wins,
    // including a value that this launcher does not understand.
    if (getenv("DOTNET_EnableWriteXorExecute") || getenv("COMPlus_EnableWriteXorExecute"))
        return NativeJitPolicy::Unchanged;
    const int saved = errno;
    int fd = memfd_create("steamohos-jit-capability", MFD_CLOEXEC);
    if (fd < 0) {
        // CoreCLR already handles mapper creation failure itself.
        errno = saved;
        return NativeJitPolicy::Unchanged;
    }
    struct stat status{};
    const int result = fstat(fd, &status), failure = errno;
    close(fd);
    errno = saved;
    if (result == 0 || (failure != EACCES && failure != EFAULT))
        return NativeJitPolicy::Unchanged;
    // FEX identifies shared aliases by the real st_dev/st_ino. On kernels
    // denying this metadata, permit CoreCLR's existing single-map JIT path.
    // The setting is inherited by this child only; it does not change FEX's
    // ARM64 JIT, graphics buffers, Steam, or the independent OHOS Wine path.
    if (setenv("DOTNET_EnableWriteXorExecute", "0", 1))
        return NativeJitPolicy::EnvironmentError;
    return NativeJitPolicy::MetadataDenied;
}
}
#endif
