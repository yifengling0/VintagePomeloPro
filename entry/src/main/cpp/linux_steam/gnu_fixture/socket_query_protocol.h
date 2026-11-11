// SPDX-License-Identifier: LGPL-2.1-or-later
// HSL b001b25 V3 record layout; VPP uses one authenticated query per channel.
#pragma once
#include <stdint.h>
#include <sys/types.h>
struct SteamOhosSocketRecord {
    int32_t error,descriptor,pid,ppid,pgid;
    uint32_t uid;
    uint64_t inode;
    char command[16];
};
typedef int (*SteamOhosSocketVisitor)(const struct SteamOhosSocketRecord *,int,void *);
#ifdef __cplusplus
extern "C" {
#endif
int SteamOhosNextTcpSocket(pid_t pid,int minimum,struct SteamOhosSocketRecord *record);
int SteamOhosVisitTcpSockets(SteamOhosSocketVisitor visitor,void *context);
#ifdef __cplusplus
}
#endif
