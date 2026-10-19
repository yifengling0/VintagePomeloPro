// Opt-in diagnostic supervisor: leave Valve's client untouched and launch a
// separate, bounded read-only helper after the real client starts.
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef STEAMOHOS_PROBE_START_MS
#define STEAMOHOS_PROBE_START_MS 20000
#endif
#ifndef STEAMOHOS_PROBE_RETRY_MS
#define STEAMOHOS_PROBE_RETRY_MS 10000
#endif
#ifndef STEAMOHOS_PROBE_TIMEOUT_MS
#define STEAMOHOS_PROBE_TIMEOUT_MS 8000
#endif
#ifndef STEAMOHOS_SOURCE_LIFETIME_MS
#define STEAMOHOS_SOURCE_LIFETIME_MS 100000
#endif

static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static pid_t launch_mode(char **argv, bool designate_client) {
    pid_t owner = getpid(), child = fork();
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner) _exit(126);
        if (designate_client) {
            char text[32];snprintf(text,sizeof(text),"%ld",(long)getpid());
            setenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID",text,1);
            setenv("STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE",argv[0],1);
        }
        execv(argv[0], argv); _exit(127);
    }
    return child;
}

static pid_t launch(char **argv) { return launch_mode(argv, false); }

// Every negative control is a fresh process/module/Steam pipe, not cached state.
static int control(const char *helper, const char *library, const char *tmp, const char *phase) {
    char file[PATH_MAX];
    if (snprintf(file, sizeof(file), "%s/steamclient-source-%ld-%s.json", tmp,
                 (long)getpid(), phase) >= (int)sizeof(file)) return -1;
    char *args[] = {(char *)helper, "--library", (char *)library, "--result-file", file, NULL};
    fprintf(stderr, "STEAMOHOS_SOURCE_CONTROL phase=%s begin=1\n", phase);
    pid_t child = launch(args);
    if (child < 0) return -1;
    uint64_t deadline = now_ms() + STEAMOHOS_PROBE_TIMEOUT_MS;
    int status;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) return -1;
        if (now_ms() >= deadline) {
            kill(child, SIGKILL); waitpid(child, NULL, 0);
            fprintf(stderr, "STEAMOHOS_SOURCE_CONTROL phase=%s timeout=1\n", phase); return -1;
        }
        poll(NULL, 0, 10);
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "STEAMOHOS_SOURCE_CONTROL phase=%s exit=%d\n", phase, code);
    return code;
}

int main(int argc, char **argv) {
    const char *root = getenv("STEAMOHOS_CODE_ROOT");
    if (argc < 2 || argv[1][0] != '/' || !root || root[0] != '/') return 2;
    const char *tmp = getenv("STEAMOHOS_TMP_ROOT");
    const char *rpc = getenv("STEAMOHOS_STEAMCLIENT_RPC");
    const char *source = getenv("STEAMOHOS_STEAMCLIENT_SOURCE_TEST");
    bool source_test = source && !strcmp(source, "1");
    bool rpc_mode = rpc && !strcmp(rpc, "1");
    if ((rpc_mode || source_test) && (!tmp || tmp[0] != '/')) return 2;
    char result_file[PATH_MAX] = {0}, endpoint[PATH_MAX] = {0};
    char helper[PATH_MAX], library[PATH_MAX];
    if (snprintf(helper, sizeof(helper), "%s/steamohos_gnu_steamclient", root) >= (int)sizeof(helper) ||
        snprintf(library, sizeof(library), "%s/steamrtarm64/steamclient.so", root) >= (int)sizeof(library)) return 2;
    if (source_test) {
        unsetenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID");
        unsetenv("STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE");
        int before = control(helper, library, tmp, "before");
        if (before != 6 && before != 7) return 15;
    }
    const char *bridge = getenv("STEAMOHOS_STEAMCLIENT_GAME_BRIDGE");
    bool bridge_mode = bridge && !strcmp(bridge,"1");
    pid_t client = launch_mode(argv + 1, true);
    if (client < 0) return 3;
    {
        // Every supervised query must connect to this original Linux Steam,
        // including the normal RPC probe when Windows Steam also exists.
        char pid[32]; snprintf(pid, sizeof(pid), "%ld", (long)client);
        setenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID", pid, 1);
        setenv("STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE", argv[1], 1);
    }
    bool verified = false, stopping = false;
    uint64_t stop_at = now_ms() + STEAMOHOS_SOURCE_LIFETIME_MS;
    pid_t probe = -1;
    unsigned attempts = bridge_mode ? 6 : 0; int status = 0;
    uint64_t next = now_ms() + STEAMOHOS_PROBE_START_MS, deadline = 0;
    for (;;) {
        pid_t result = waitpid(client, &status, WNOHANG);
        if (result == client) break;
        if (result < 0 && errno != EINTR) { kill(client, SIGKILL); waitpid(client, NULL, 0); return 4; }
        uint64_t current = now_ms();
        if (source_test && current >= stop_at) {
            fprintf(stderr, "STEAMOHOS_SOURCE_CONTROL phase=stop intentional=1 signal=%d\n", stopping ? SIGKILL : SIGTERM);
            kill(client, stopping ? SIGKILL : SIGTERM);
            stopping = true; stop_at = current + 2000;
        }
        if (probe > 0) {
            int probe_status = 0;
            result = waitpid(probe, &probe_status, WNOHANG);
            if (result == probe || (result < 0 && errno == ECHILD)) {
                int code = result == probe && WIFEXITED(probe_status) ? WEXITSTATUS(probe_status) : -1;
                fprintf(stderr, "GNU_STEAMCLIENT_HELPER attempt=%u exit=%d\n", attempts, code);
                probe = -1;
                if (*endpoint) unlink(endpoint);
                if (!code) { attempts = 6; verified = true; }
                next = current + STEAMOHOS_PROBE_RETRY_MS;
            } else if (current >= deadline) {
                kill(probe, SIGKILL); waitpid(probe, NULL, 0); probe = -1;
                if (*endpoint) unlink(endpoint);
                fputs("GNU_STEAMCLIENT_HELPER timeout\n", stderr);
                next = current + STEAMOHOS_PROBE_RETRY_MS;
            }
        }
        if (probe < 0 && attempts < 6 && current >= next) {
            ++attempts;
            char *args[9] = {helper, "--library", library, NULL};
            unsigned count = 3;
            if (tmp && tmp[0] == '/') {
                if (snprintf(result_file, sizeof(result_file), "%s/steamclient-readonly-%ld-%u.json", tmp,
                             (long)getpid(), attempts) >= (int)sizeof(result_file) ||
                    snprintf(endpoint, sizeof(endpoint), "%s/steamclient-rpc-%ld-%u.endpoint", tmp,
                             (long)getpid(), attempts) >= (int)sizeof(endpoint)) {
                    kill(client, SIGKILL); waitpid(client, NULL, 0); return 2;
                }
                args[count++] = "--result-file"; args[count++] = result_file;
                if (rpc_mode) { args[count++] = "--serve"; args[count++] = endpoint; }
                args[count] = NULL;
            }
            probe = launch(args);
            deadline = current + (rpc_mode ? 120000 : STEAMOHOS_PROBE_TIMEOUT_MS);
            if (probe < 0) { kill(client, SIGKILL); waitpid(client, NULL, 0); return 5; }
        }
        poll(NULL, 0, 50);
    }
    if (probe > 0) { kill(probe, SIGKILL); waitpid(probe, NULL, 0); }
    if (*endpoint) unlink(endpoint);
    if (source_test) {
        unsetenv("STEAMOHOS_STEAMCLIENT_EXPECTED_PID");
        unsetenv("STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE");
        int after = control(helper, library, tmp, "after");
        fprintf(stderr, "STEAMOHOS_SOURCE_CONTROL phase=summary sourceVerified=%d afterExit=%d intentionalStop=%d\n", verified, after, stopping);
        return verified && (after == 6 || after == 7) ? 0 : 16;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) { signal(WTERMSIG(status), SIG_DFL); raise(WTERMSIG(status)); }
    return 6;
}
