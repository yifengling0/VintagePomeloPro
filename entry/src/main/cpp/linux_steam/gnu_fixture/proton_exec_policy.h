#ifndef HMOSDECK_PROTON_EXEC_POLICY_H
#define HMOSDECK_PROTON_EXEC_POLICY_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

/* The OHOS JIT policy belongs to a thread and must be re-established after
 * fork. Wine/FEX may change the per-thread mode between calls, so refresh it
 * for each executable transition, matching the native OHOS Wine path. */
static void proton_prepare_exec_policy(int prot) {
    if (!(prot & PROT_EXEC)) return;
    const char *root = getenv("STEAMOHOS_CODE_ROOT");
    if (!root || strcmp(root, "/data/storage/el2/base/files/steamohos-native/proton-probe")) return;
    const int saved = errno;
    (void)prctl(0x6a6974, 0UL, 0UL, 0UL, 0UL);
    errno = saved;
}
#endif
