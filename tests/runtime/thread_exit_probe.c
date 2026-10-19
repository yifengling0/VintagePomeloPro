#include <windows.h>
#include <stdio.h>
#include <string.h>

struct wait_context { HANDLE ready, stop; volatile LONG spin; };
struct main_exit_context { HANDLE main_thread, ready, stop; FILE *log; };
static struct main_exit_context main_exit;

static DWORD WINAPI terminate_main_thread(void *arg)
{
    struct main_exit_context *context = arg;
    DWORD code = 0;
    if (WaitForSingleObject(context->ready, 10000) != WAIT_OBJECT_0) {
        fprintf(context->log, "FAIL main ready error=%lu\n", GetLastError());
        return 10;
    }
    Sleep(5);
    if (!TerminateThread(context->main_thread, 42)
        || WaitForSingleObject(context->main_thread, 10000) != WAIT_OBJECT_0
        || !GetExitCodeThread(context->main_thread, &code) || code != 42) {
        fprintf(context->log, "FAIL main terminate code=%lu error=%lu\n", code, GetLastError());
        return 11;
    }
    CloseHandle(context->main_thread);
    CloseHandle(context->ready);
    CloseHandle(context->stop);
    /* Keep a survivor alive while the initial native thread finishes cleanup. */
    Sleep(250);
    fprintf(context->log, "PASS mode=forced-main completed=1 failures=0\n");
    fclose(context->log);
    return 0;
}

static DWORD WINAPI returning_thread(void *arg)
{
    (void)arg;
    return 42;
}

static DWORD WINAPI waiting_thread(void *arg)
{
    struct wait_context *context = arg;
    SetEvent(context->ready);
    WaitForSingleObject(context->stop, INFINITE);
    return 99;
}

static DWORD WINAPI exiting_thread(void *arg)
{
    (void)arg;
    ExitThread(42);
    return 0;
}

static DWORD WINAPI spinning_thread(void *arg)
{
    struct wait_context *context = arg;
    SetEvent(context->ready);
    for (;;) context->spin++;
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "normal";
    const char *path = argc > 2 ? argv[2] : "C:\\vp-thread-exit.txt";
    FILE *log = fopen(path, "w");
    int forced_user = strcmp(mode, "forced-user") == 0;
    int explicit_exit = strcmp(mode, "exit") == 0;
    int forced = forced_user || strcmp(mode, "forced") == 0;
    int count = forced ? 32 : 256;
    struct wait_context context = {0};
    LPTHREAD_START_ROUTINE procedure = forced_user ? spinning_thread :
        forced ? waiting_thread : explicit_exit ? exiting_thread : returning_thread;
    if (!log) return 2;
    setvbuf(log, NULL, _IONBF, 0);
    if (strcmp(mode, "forced-main") == 0) {
        HANDLE terminator;
        fprintf(log, "VP-THREAD-EXIT mode=forced-main pid=%lu iterations=1\n", GetCurrentProcessId());
        main_exit.log = log;
        main_exit.main_thread = OpenThread(THREAD_TERMINATE | SYNCHRONIZE | THREAD_QUERY_INFORMATION,
                                          FALSE, GetCurrentThreadId());
        main_exit.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        main_exit.stop = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!main_exit.main_thread || !main_exit.ready || !main_exit.stop) {
            fprintf(log, "FAIL main setup error=%lu\n", GetLastError());
            return 12;
        }
        terminator = CreateThread(NULL, 0, terminate_main_thread, &main_exit, 0, NULL);
        if (!terminator) {
            fprintf(log, "FAIL main worker error=%lu\n", GetLastError());
            return 13;
        }
        CloseHandle(terminator);
        SetEvent(main_exit.ready);
        WaitForSingleObject(main_exit.stop, INFINITE);
        fprintf(log, "FAIL main unexpectedly returned from infinite wait\n");
        return 14;
    }
    if (!forced && !explicit_exit && strcmp(mode, "normal") != 0) {
        fprintf(log, "FAIL unknown mode=%s\n", mode);
        return 8;
    }
    fprintf(log, "VP-THREAD-EXIT mode=%s pid=%lu iterations=%d\n", mode, GetCurrentProcessId(), count);
    context.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    context.stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!context.ready || !context.stop) {
        fprintf(log, "FAIL events error=%lu\n", GetLastError());
        return 3;
    }
    for (int i = 0; i < count; ++i) {
        DWORD code;
        HANDLE thread;
        ResetEvent(context.ready);
        thread = CreateThread(NULL, 0, procedure, &context, 0, NULL);
        if (!thread) {
            fprintf(log, "FAIL create iteration=%d error=%lu\n", i, GetLastError());
            return 4;
        }
        if (forced) {
            if (WaitForSingleObject(context.ready, 10000) != WAIT_OBJECT_0) {
                fprintf(log, "FAIL ready iteration=%d error=%lu\n", i, GetLastError());
                return 5;
            }
            /* Allow entry into either the Unix wait or the user-mode loop. */
            Sleep(5);
            if (!TerminateThread(thread, 42)) {
                fprintf(log, "FAIL terminate iteration=%d error=%lu\n", i, GetLastError());
                return 6;
            }
        }
        if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0
            || !GetExitCodeThread(thread, &code) || code != 42) {
            fprintf(log, "FAIL join iteration=%d error=%lu\n", i, GetLastError());
            return 7;
        }
        CloseHandle(thread);
        /* Windows signals the thread before native pthread cleanup finishes. */
        Sleep(2);
    }
    CloseHandle(context.ready);
    CloseHandle(context.stop);
    fprintf(log, "PASS mode=%s completed=%d failures=0\n", mode, count);
    fclose(log);
    return 0;
}
