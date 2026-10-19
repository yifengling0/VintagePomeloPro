#ifndef STEAMOHOS_STEAMCLIENT_PROBE_H
#define STEAMOHOS_STEAMCLIENT_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool SteamOhosVerifyClientSource(const char *, void *, char *, size_t);
int SteamOhosReadOnlyQuery(const char *, uint32_t, char *, size_t, uint32_t *);
int SteamOhosReadOnlyServe(const char *, const char *);
// Diagnostic snapshot only; this does not implement SteamAPI_Init or tickets.
// Request: HDSQ, version u16, opcode u16, request ID u32, AppID u32, nonce[32].
// Response: HDSR, version u16, result u16, request ID u32, AppID u32, flags u32,
//           reserved zero bytes[12]. Integers use network byte order.
#define STEAMOHOS_QUERY_BYTES 48
#define STEAMOHOS_REPLY_BYTES 32
static inline void steamohos_put32(unsigned char *p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
static inline uint32_t steamohos_get32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
#endif
