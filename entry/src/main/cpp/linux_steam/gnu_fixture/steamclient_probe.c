// Read-only GNU Steam client ABI probe. No credentials, account IDs, or tickets
// leave this process. Fixed public interface versions; no guessed fallback ABI.
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "steamclient_probe.h"

// ISteamClient020, ISteamUser021 and ISteamApps008 public vtable contracts.
// See docs/linux-steam/STEAMCLIENT_PROBE.zh-CN.md for pinned ABI references.
static void *method(void *object, unsigned slot) {
    return (*(void ***)object)[slot];
}
int SteamOhosReadOnlyQuery(const char *library, uint32_t application,
                          char *output, size_t capacity, uint32_t *flags) {
    const char *status = "library_load_failed";
    bool loaded = false, client_ok = false, logged_on = false, identity_present = false;
    bool apps_ok = false, subscribed = false, user_release = false, pipe_release = false;
    bool callbacks_available = false, source_verified = false;
    bool source_required = getenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID") != NULL;
    char source_report[384] = "{}";
    int pipe = 0, user = 0, code = 3;
    void *client = NULL;
    static void *module;
    if (!module) module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!module) goto done;
    loaded = true;
    void *(*factory)(const char *, int *) = (void *(*)(const char *, int *))dlsym(module, "CreateInterface");
    if (!factory) { status = "factory_unavailable"; code = 4; goto done; }
    callbacks_available = dlsym(module, "Steam_BGetCallback") &&
        dlsym(module, "Steam_FreeLastCallback") && dlsym(module, "Steam_GetAPICallResult");
    client = factory("SteamClient020", NULL);
    if (!client) { status = "client_interface_unavailable"; code = 5; goto done; }
    client_ok = true;
    pipe = ((int (*)(void *))method(client, 0))(client);
    if (!pipe) { status = "pipe_unavailable"; code = 6; goto done; }
    user = ((int (*)(void *, int))method(client, 2))(client, pipe);
    if (!user) { status = "user_unavailable"; code = 7; goto cleanup; }
    void *account = ((void *(*)(void *, int, int, const char *))method(client, 5))
        (client, user, pipe, "SteamUser021");
    if (!account) { status = "user_interface_unavailable"; code = 8; goto cleanup; }
    logged_on = ((bool (*)(void *))method(account, 1))(account);
    identity_present = ((uint64_t (*)(void *))method(account, 2))(account) != 0;
    void *apps = ((void *(*)(void *, int, int, const char *))method(client, 15))
        (client, user, pipe, "STEAMAPPS_INTERFACE_VERSION008");
    apps_ok = apps != NULL;
    if (!apps) { status = "apps_interface_unavailable"; code = 9; goto cleanup; }
    if (application) subscribed = ((bool (*)(void *, uint32_t))method(apps, 6))(apps, application);
    if (!logged_on) { status = "steam_offline"; code = 10; goto cleanup; }
    if (!identity_present) { status = "user_identity_unavailable"; code = 11; goto cleanup; }
    if (source_required) {
        source_verified = SteamOhosVerifyClientSource(library, (void *)factory, source_report, sizeof(source_report));
        if (!source_verified) { status = "client_source_unverified"; code = 14; goto cleanup; }
    }
    status = "steamclient_readonly_verified"; code = 0;
cleanup:
    if (user) {
        ((void (*)(void *, int, int))method(client, 4))(client, pipe, user);
        user_release = true;
    }
    pipe_release = ((bool (*)(void *, int))method(client, 1))(client, pipe);
    if (!pipe_release) { status = "pipe_release_failed"; code = 12; }
done:
    // As in Proton, keep steamclient loaded for the lifetime of this process:
    // unloading its code while internal worker threads exist is unsafe.
    *flags = (loaded << 0) | (client_ok << 1) | ((pipe != 0) << 2) | ((user != 0) << 3) |
        (logged_on << 4) | (identity_present << 5) | (apps_ok << 6) |
        ((application && apps_ok) << 7) | (subscribed << 8) | (callbacks_available << 9) |
        (user_release << 10) | (pipe_release << 11) | (source_verified << 12);
    snprintf(output, capacity, "{\"schemaVersion\":1,\"status\":\"%s\","
        "\"libraryLoaded\":%s,\"clientInterfaceVerified\":%s,\"pipeCreated\":%s,"
        "\"userConnected\":%s,\"loggedOn\":%s,\"userIdentityPresent\":%s,"
        "\"appsInterfaceVerified\":%s,\"ownershipQueried\":%s,\"appId\":%u,\"subscribed\":%s,"
        "\"callbackExportsAvailable\":%s,\"userReleaseCalled\":%s,\"pipeReleaseSucceeded\":%s,"
        "\"source\":%s,"
        "\"clientSourceRequired\":%s,\"clientSourceVerified\":%s,"
        "\"steamApiInitVerified\":false,\"authenticationVerified\":false}\n",
        status, loaded ? "true" : "false", client_ok ? "true" : "false",
        pipe ? "true" : "false", user ? "true" : "false", logged_on ? "true" : "false",
        identity_present ? "true" : "false", apps_ok ? "true" : "false",
        application && apps_ok ? "true" : "false", application, subscribed ? "true" : "false",
        callbacks_available ? "true" : "false", user_release ? "true" : "false",
        pipe_release ? "true" : "false", source_report, source_required ? "true" : "false", source_verified ? "true" : "false");
    return code;
}

#ifndef STEAMOHOS_PROBE_NO_MAIN
static int appid(const char *text, uint32_t *value) {
    if (!text || !*text || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0; char *end; unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || !n || n > UINT32_MAX) return -1;
    *value = (uint32_t)n; return 0;
}
int main(int argc, char **argv) {
    const char *library = NULL, *result_file = NULL, *endpoint = NULL;
    uint32_t application = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--library") && !library && i + 1 < argc)
            library = argv[++i];
        else if (!strcmp(argv[i], "--result-file") && !result_file && i + 1 < argc)
            result_file = argv[++i];
        else if (!strcmp(argv[i], "--serve") && !endpoint && i + 1 < argc)
            endpoint = argv[++i];
        else if (!strcmp(argv[i], "--appid") && !application && i + 1 < argc) {
            if (appid(argv[++i], &application)) return 2;
        } else return 2;
    }
    if (!library || library[0] != '/' || strlen(library) >= PATH_MAX ||
        (result_file && (result_file[0] != '/' || strlen(result_file) >= PATH_MAX)) ||
        (endpoint && (endpoint[0] != '/' || strlen(endpoint) >= PATH_MAX))) return 2;
    char result[2048]; uint32_t flags = 0;
    int code = SteamOhosReadOnlyQuery(library, application, result, sizeof(result), &flags);
    fputs("STEAMOHOS_STEAMCLIENT_RESULT ", stdout); fputs(result, stdout); fflush(stdout);
    if (result_file) {
        int fd = open(result_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return 13;
        size_t length = strlen(result), offset = 0;
        while (offset < length) {
            ssize_t n = write(fd, result + offset, length - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return 13; }
            offset += (size_t)n;
        }
        if (close(fd)) return 13;
    }
    if (endpoint) {
        int served = SteamOhosReadOnlyServe(library, endpoint);
        if (served) return served;
    }
    return code;
}
#endif
