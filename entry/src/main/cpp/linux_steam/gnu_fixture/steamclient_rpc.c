// Versioned, nonce-authenticated loopback transport for the diagnostic PE
// probe. No pointers or native handles cross the process/libc boundary.
#define _GNU_SOURCE 1
#include "steamclient_probe.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef STEAMOHOS_RPC_LIFETIME_MS
#define STEAMOHOS_RPC_LIFETIME_MS 90000
#endif
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static bool exact_io(int fd, unsigned char *bytes, size_t count, bool write_mode, uint64_t deadline) {
    size_t offset = 0;
    while (offset < count && now_ms() < deadline) {
        ssize_t n = write_mode ? send(fd, bytes + offset, count - offset, MSG_NOSIGNAL) :
                                 recv(fd, bytes + offset, count - offset, 0);
        if (n > 0) { offset += (size_t)n; continue; }
        if (!n) return false;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
        struct pollfd p = {fd, write_mode ? POLLOUT : POLLIN, 0};
        if (poll(&p, 1, 20) < 0 && errno != EINTR) return false;
    }
    return offset == count;
}
int SteamOhosReadOnlyServe(const char *library, const char *endpoint) {
    unsigned char nonce[32]; size_t obtained = 0;
    while (obtained < sizeof(nonce)) {
        ssize_t n = getrandom(nonce + obtained, sizeof(nonce) - obtained, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 14;
        obtained += (size_t)n;
    }
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server < 0) return 14;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    if (bind(server, (struct sockaddr *)&address, size) || listen(server, 4) ||
        getsockname(server, (struct sockaddr *)&address, &size)) { close(server); return 14; }
    int fd = open(endpoint, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { close(server); return 14; }
    char text[128], hex[65];
    for (unsigned i = 0; i < sizeof(nonce); ++i) snprintf(hex + 2 * i, 3, "%02x", nonce[i]);
    int length = snprintf(text, sizeof(text), "HMOSDECK/1\n%u\n%s\n", ntohs(address.sin_port), hex);
    ssize_t written = write(fd, text, (size_t)length); int closed = close(fd);
    if (written != length || closed) { close(server); unlink(endpoint); return 14; }
    fputs("GNU_STEAMCLIENT_RPC ready version=1 bound=loopback\n", stderr);
    unsigned accepted = 0, verified = 0;
    uint64_t deadline = now_ms() + STEAMOHOS_RPC_LIFETIME_MS;
    while (now_ms() < deadline && accepted < 32) {
        struct pollfd wait = {server, POLLIN, 0};
        if (poll(&wait, 1, 50) <= 0) continue;
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0) continue;
        ++accepted;
        unsigned char request[STEAMOHOS_QUERY_BYTES], response[STEAMOHOS_REPLY_BYTES] = {0};
        uint64_t until = now_ms() + 1500;
        if (until > deadline) until = deadline;
        bool valid = exact_io(client, request, sizeof(request), false, until);
        unsigned difference = 0;
        if (valid) for (unsigned i = 0; i < sizeof(nonce); ++i) difference |= nonce[i] ^ request[16 + i];
        if (valid && !difference && !memcmp(request, "HDSQ\0\1\0\1", 8)) {
            uint32_t app = steamohos_get32(request + 12), flags = 0;
            char result[2048]; int code = SteamOhosReadOnlyQuery(library, app, result, sizeof(result), &flags);
            memcpy(response, "HDSR\0\1", 6); response[7] = (unsigned char)code;
            memcpy(response + 8, request + 8, 8); steamohos_put32(response + 16, flags);
            if (exact_io(client, response, sizeof(response), true, until)) ++verified;
        }
        close(client);
    }
    close(server); unlink(endpoint);
    fprintf(stderr, "GNU_STEAMCLIENT_RPC finished accepted=%u replied=%u\n", accepted, verified);
    return 0;
}
