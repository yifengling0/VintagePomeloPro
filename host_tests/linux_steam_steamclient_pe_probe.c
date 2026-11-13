// A diagnostic Windows program for the CURRENT OHOS Wine runtime. This is
// deliberately not steam_api64.dll and does not claim SteamAPI_Init support.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/steamclient_probe.h"
static int exact(SOCKET s, unsigned char *bytes, int length, int writing) {
    int offset = 0;
    while (offset < length) {
        int n = writing ? send(s, (const char *)bytes + offset, length - offset, 0) :
                          recv(s, (char *)bytes + offset, length - offset, 0);
        if (n <= 0) return 0;
        offset += n;
    }
    return 1;
}
static int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static unsigned windows_steam_processes(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 entry = {0}; entry.dwSize = sizeof(entry);
    unsigned count = 0;
    if (Process32First(snapshot, &entry)) do {
        if (!_stricmp(entry.szExeFile, "steam.exe")) ++count;
    } while (Process32Next(snapshot, &entry));
    CloseHandle(snapshot); return count;
}
int main(void) {
    const char *status = "endpoint_unavailable";
    FILE *f = fopen("C:/windows/temp/hmosdeck-steamclient.endpoint", "rb");
    char header[32], port_text[16], nonce[80];
    unsigned char request[STEAMOHOS_QUERY_BYTES] = {0}, response[STEAMOHOS_REPLY_BYTES] = {0};
    SOCKET connection = INVALID_SOCKET;
    WSADATA wsa; int started = 0, code = 1, native_code = -1; uint32_t flags = 0;
    if (!f) goto done;
    int parsed = fgets(header, sizeof(header), f) && fgets(port_text, sizeof(port_text), f) && fgets(nonce, sizeof(nonce), f);
    fclose(f);
    if (!parsed || strcmp(header, "HMOSDECK/1\n") || strlen(nonce) != 65 || nonce[64] != '\n') {
        status = "endpoint_invalid"; goto done;
    }
    char *end; unsigned long port = strtoul(port_text, &end, 10);
    if (!port || port > 65535 || strcmp(end, "\n")) { status = "endpoint_invalid"; goto done; }
    memcpy(request, "HDSQ\0\1\0\1", 8); steamohos_put32(request + 8, 1);
    // AppID 0 means no ownership query, never an invented game context.
    steamohos_put32(request + 12, 0);
    for (unsigned i = 0; i < 32; ++i) {
        int a = digit(nonce[2*i]), b = digit(nonce[2*i+1]);
        if (a < 0 || b < 0) { status = "endpoint_invalid"; goto done; }
        request[16+i] = (unsigned char)(a*16+b);
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { status = "winsock_unavailable"; goto done; }
    started = 1;
    connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) { status = "socket_failed"; goto done; }
    DWORD timeout = 2000;
    if (setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) ||
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout))) {
        status = "socket_options_failed"; goto done;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address))) { status = "connect_failed"; goto done; }
    if (!exact(connection, request, sizeof(request), 1) || !exact(connection, response, sizeof(response), 0)) {
        status = "transport_failed"; goto done;
    }
    if (memcmp(response, "HDSR\0\1", 6) || steamohos_get32(response + 8) != 1 || steamohos_get32(response + 12) != 0) {
        status = "response_invalid"; goto done;
    }
    for (unsigned i = 20; i < sizeof(response); ++i)
        if (response[i]) { status = "response_invalid"; goto done; }
    native_code = response[6] * 256 + response[7]; flags = steamohos_get32(response + 16);
    if (native_code) { status = "native_query_failed"; goto done; }
    if (!(flags & (1u << 12))) { status = "native_source_unverified"; goto done; }
    if ((flags & 0xc7f) != 0xc7f) { status = "native_result_incomplete"; goto done; }
    status = "wine_gnu_readonly_rpc_verified"; code = 0;
done:
    if (connection != INVALID_SOCKET) closesocket(connection);
    if (started) WSACleanup();
    char result[768];
    snprintf(result, sizeof(result), "{\"schemaVersion\":1,\"status\":\"%s\",\"nativeCode\":%d,"
        "\"probeProcessId\":%lu,\"probeRunMilliseconds\":%llu,\"loggedOn\":%s,\"userIdentityPresent\":%s,\"pipeReleaseSucceeded\":%s,"
        "\"clientSourceVerified\":%s,\"windowsSteamProcessCount\":%u,"
        "\"steamApiInitVerified\":false,\"authenticationVerified\":false}\n", status, native_code, (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(),
        flags & (1u<<4) ? "true" : "false", flags & (1u<<5) ? "true" : "false", flags & (1u<<11) ? "true" : "false", flags & (1u<<12) ? "true" : "false", windows_steam_processes());
    f = fopen("C:/windows/temp/hmosdeck-steamclient-result.json", "wb");
    if (!f) return 2;
    size_t length = strlen(result); int written = fwrite(result, 1, length, f) == length;
    if (fclose(f) || !written) return 2;
    fputs(result, stdout);
    return code;
}
