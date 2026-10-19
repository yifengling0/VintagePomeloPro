/* A real Wine window. GNU Steam remains native; no Steam/Valve code is linked. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t state_path[2048];
static unsigned activation;
static char previous[512];
static unsigned control_serial;
static int presentation;
static RECT windowed_bounds;

/* Only presentation state crosses this boundary; never pixels or commands. */
static void control(HWND window)
{
    wchar_t path[2100]; swprintf(path, 2100, L"%ls.control", state_path);
    FILE *file = _wfopen(path, L"rt");
    if (!file) return;
    unsigned serial; int mode, minimized, activate;
    int count = fscanf(file, "%u %d %d %d", &serial, &mode, &minimized, &activate);
    fclose(file);
    if (count != 4 || serial == control_serial || mode < 0 || mode > 2 ||
        minimized < 0 || minimized > 1 || activate < 0 || activate > 1) return;
    control_serial = serial;
    if (mode != presentation) {
        if (presentation == 0) GetWindowRect(window, &windowed_bounds);
        presentation = mode;
        if (mode == 0) {
            ShowWindow(window, SW_RESTORE);
            SetWindowPos(window, NULL, windowed_bounds.left, windowed_bounds.top,
                windowed_bounds.right-windowed_bounds.left, windowed_bounds.bottom-windowed_bounds.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            MONITORINFO monitor = { .cbSize = sizeof(monitor) };
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            RECT area = mode == 2 ? monitor.rcMonitor : monitor.rcWork;
            ShowWindow(window, SW_RESTORE);
            SetWindowPos(window, NULL, area.left, area.top, area.right-area.left,
                area.bottom-area.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (minimized) ShowWindow(window, SW_MINIMIZE);
    else if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    if (activate && !minimized) SetForegroundWindow(window);
}

static void publish(HWND window, int closed)
{
    RECT outer = {0}, client = {0}; POINT origin = {0};
    GetWindowRect(window, &outer); GetClientRect(window, &client);
    ClientToScreen(window, &origin);
    char text[512];
    snprintf(text, sizeof(text),
        "{\"version\":1,\"x\":%ld,\"y\":%ld,\"width\":%ld,\"height\":%ld,"
        "\"minimized\":%d,\"activation\":%u,\"closed\":%d,\"mode\":%d}\n",
        origin.x - outer.left, origin.y - outer.top,
        client.right, client.bottom, !!IsIconic(window), activation, closed, presentation);
    if (!strcmp(previous, text)) return;
    wchar_t temporary[2100]; swprintf(temporary, 2100, L"%ls.tmp", state_path);
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0; BOOL ok = WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(file);
    if (ok && written == strlen(text) && MoveFileExW(temporary, state_path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) strcpy(previous, text);
}

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) ++activation;
        publish(window, 0); break;
    case WM_TIMER:
        control(window); publish(window, 0); break;
    case WM_MOVE: case WM_SIZE:
        publish(window, 0); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(dc, RGB(215, 225, 235)); SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 24, 24, L"Starting Steam (Linux ARM64)...", 31);
        EndPaint(window, &paint); return 0;
    }
    case WM_CLOSE:
        publish(window, 1); DestroyWindow(window); return 0;
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance, PWSTR command, int show)
{
    (void)previous_instance; (void)command; (void)show;
    /* Host supplies a fresh per-launch file; no arbitrary command execution. */
    if (!GetEnvironmentVariableW(L"HMOSDECK_WINDOW_STATE", state_path, 2048) ||
        wcslen(state_path) > 2000) return 2;
    WNDCLASSW type = {0}; type.lpfnWndProc = procedure; type.hInstance = instance;
    type.lpszClassName = L"HmosDeckSteamHostV1";
    type.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&type)) return 3;
    RECT bounds = {0, 0, 1024, 640};
    /* Steam already draws its own frame. Keep a real taskbar HWND without
       adding a second caption or border around the original client. */
    DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName,
        L"Steam (Linux ARM64)", style,
        60, 35, bounds.right - bounds.left, bounds.bottom - bounds.top,
        NULL, NULL, instance, NULL);
    if (!window) return 4;
    GetWindowRect(window, &windowed_bounds);
    SetTimer(window, 1, 150, NULL); ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window); publish(window, 0);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
