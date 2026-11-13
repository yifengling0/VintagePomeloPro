// SPDX-License-Identifier: MIT
// Used ONLY by FEX's guest syscall handler, never by its ARM64 JIT allocator.
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
namespace FEX::Ohos {
inline int GuestMprotect(void* address, size_t length, int guest_prot) {
  int result = ::mprotect(address, length, guest_prot);
  const int original_errno = errno;
  const char* scope = std::getenv("STEAMOHOS_NATIVE_FEX");
  if (result == -1 && (original_errno == EACCES || original_errno == EPERM) &&
      (guest_prot & PROT_EXEC) && scope && !std::strcmp(scope, "1")) {
    // The x86 bytes are read by the translator. Keep the guest's original
    // protection in FEX VMA tracking; do not request host execution of them.
    result = ::mprotect(address, length, guest_prot & ~PROT_EXEC);
    const int retry_errno = errno;
    const char* diagnostic = std::getenv("STEAMOHOS_NATIVE_MEMORY");
    static std::atomic<unsigned> reports{0};
    if (diagnostic && !std::strcmp(diagnostic, "1") && reports.fetch_add(1) < 16)
      std::fprintf(stderr, "STEAMOHOS_FEX_GUEST_PROTECTION original_errno=%d retry_ok=%d guest_exec=1 host_exec=0\n",
                   original_errno, result == 0);
    errno = retry_errno;
  } else {
    errno = original_errno;
  }
  return result;
}
}
