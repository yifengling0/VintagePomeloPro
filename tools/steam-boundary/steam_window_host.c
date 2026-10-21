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
static int applying_control;
static unsigned applied_serial;
static RECT restore_target;
static int restore_pending, restore_matches, restore_retries;

/* Wine/Wayland can finish an earlier configure after SetWindowPos. Publish
 * initial placement and every presentation change only after geometry settles. */
static void settle_restore(HWND window)
{
    if (restore_pending && !IsIconic(window)) {
        RECT actual; GetWindowRect(window, &actual);
        if (EqualRect(&actual, &restore_target)) {
            if (++restore_matches >= 2) restore_pending = 0;
        } else {
            restore_matches = 0;
            if (restore_retries < 20) {
                ++restore_retries;
                applying_control = 1;
                SetWindowPos(window, NULL, restore_target.left, restore_target.top,
                    restore_target.right-restore_target.left, restore_target.bottom-restore_target.top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                applying_control = 0;
            }
        }
    }
    if (!restore_pending) applied_serial = control_serial;
}

static RECT work_area(HWND window, int fullscreen)
{
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    RECT area = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR output = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) :
        MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(output, &monitor)) area = fullscreen ? monitor.rcMonitor : monitor.rcWork;
    return area;
}

static RECT fit_bounds(RECT bounds, RECT area)
{
    int aw = area.right-area.left, ah = area.bottom-area.top;
    int width = bounds.right-bounds.left, height = bounds.bottom-bounds.top;
    if (width > aw) width = aw;
    if (height > ah) height = ah;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (bounds.left < area.left) bounds.left = area.left;
    if (bounds.top < area.top) bounds.top = area.top;
    if (bounds.left+width > area.right) bounds.left = area.right-width;
    if (bounds.top+height > area.bottom) bounds.top = area.bottom-height;
    bounds.right = bounds.left+width; bounds.bottom = bounds.top+height;
    return bounds;
}

static RECT initial_bounds(void)
{
    RECT area = work_area(NULL, 0);
    int available_w = area.right-area.left, available_h = area.bottom-area.top;
    /* The Wine work area is the launch surface, not the panel's physical pixels.
       Keep a desktop margin and cap the CEF surface to avoid excess software work. */
    int width = available_w * 9 / 10, height = available_h * 9 / 10;
    if (width > 1024) width = 1024;
    if (height > 640) height = 640;
    wchar_t saved[32],extra;int saved_w,saved_h,restored=0;
    if (GetEnvironmentVariableW(L"HMOSDECK_WINDOW_SIZE",saved,32)>0&&
        swscanf(saved,L"%dx%d%c",&saved_w,&saved_h,&extra)==2&&
        saved_w>=320&&saved_w<=4096&&saved_h>=200&&saved_h<=4096&&
        (long long)saved_w*saved_h<=8388608) {
        width=saved_w; height=saved_h; restored=1;
    }
    if (!restored) {
        if (width * 5 > height * 8) width = height * 8 / 5;
        else height = width * 5 / 8;
    }
    int x = area.left + (available_w-width)/2;
    int y = area.top + (available_h-height)/2;
    return fit_bounds((RECT){x,y,x+width,y+height}, area);
}

/* Only presentation state crosses this boundary; never pixels or commands. */
static void control(HWND window)
{
    wchar_t path[2100]; swprintf(path, 2100, L"%ls.control", state_path);
    FILE *file = _wfopen(path, L"rt");
    if (!file) return;
    unsigned serial; int mode, minimized, activate,normal_width=0,normal_height=0;
    int count = fscanf(file, "%u %d %d %d %d %d", &serial, &mode, &minimized, &activate,&normal_width,&normal_height);
    fclose(file);
    if ((count != 4 && count != 6) || serial == control_serial || mode < 0 || mode > 2 ||
        minimized < 0 || minimized > 1 || activate < 0 || activate > 1) return;
    int dimensions=count==6&&normal_width>=320&&normal_width<=4096&&normal_height>=200&&normal_height<=4096&&
        (long long)normal_width*normal_height<=8388608;
    RECT actual;GetWindowRect(window,&actual);
    int resize=mode==0&&dimensions&&(windowed_bounds.right-windowed_bounds.left!=normal_width||
        windowed_bounds.bottom-windowed_bounds.top!=normal_height||
        actual.right-actual.left!=normal_width||actual.bottom-actual.top!=normal_height);
    applying_control = 1;
    control_serial = serial;
    if (mode != presentation || resize) {
        if (presentation == 0 && !IsIconic(window)) GetWindowRect(window, &windowed_bounds);
        if(dimensions) {
            windowed_bounds.right=windowed_bounds.left+normal_width;
            windowed_bounds.bottom=windowed_bounds.top+normal_height;
        }
        presentation = mode;
        RECT area = work_area(window, mode == 2);
        RECT target = mode == 0 ? fit_bounds(windowed_bounds, area) : area;
        restore_pending = 1; restore_matches = 0; restore_retries = 0;
        restore_target = target;
        if (mode == 0) windowed_bounds = target;
        /* SW_RESTORE on an already normal borderless HWND restores Wine's old
           placement a second time. Only uniconify when it is actually required. */
        if (IsIconic(window) || IsZoomed(window)) ShowWindow(window, SW_RESTORE);
        SetWindowPos(window, NULL, target.left, target.top,
            target.right-target.left, target.bottom-target.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (minimized) ShowWindow(window, SW_MINIMIZE);
    else if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    if (activate && !minimized) SetForegroundWindow(window);
    applying_control = 0;
    if (!restore_pending) applied_serial = control_serial;
}

static void publish(HWND window, int closed)
{
    if (applying_control) return;
    RECT outer = {0}, client = {0}; POINT origin = {0};
    GetWindowRect(window, &outer); GetClientRect(window, &client);
    ClientToScreen(window, &origin);
    char text[512];
    snprintf(text, sizeof(text),
        "{\"version\":1,\"x\":%ld,\"y\":%ld,\"width\":%ld,\"height\":%ld,"
        "\"minimized\":%d,\"activation\":%u,\"closed\":%d,\"mode\":%d,\"controlSerial\":%u,\"outerX\":%ld,\"outerY\":%ld,\"geometryReady\":%d,\"restoreRetries\":%d}\n",
        origin.x - outer.left, origin.y - outer.top,
        client.right, client.bottom, !!IsIconic(window), activation, closed, presentation, applied_serial, outer.left, outer.top, !restore_pending, restore_retries);
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
        control(window); settle_restore(window); publish(window, 0); break;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: {
        RECT target = presentation ? work_area(window, presentation == 2) :
            fit_bounds(windowed_bounds, work_area(window, 0));
        if (!presentation) windowed_bounds = target;
        /* Rotation replaces the work area while an earlier configure may still
           be pending. Do not keep retrying a target from the old orientation. */
        restore_target = target; restore_pending = 1;
        restore_matches = 0; restore_retries = 0;
        applying_control = 1;
        SetWindowPos(window, NULL, target.left, target.top, target.right-target.left,
            target.bottom-target.top, SWP_NOZORDER | SWP_NOACTIVATE);
        applying_control = 0;
        publish(window, 0); break;
    }
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
    RECT bounds = initial_bounds();
    /* Steam already draws its own frame. Keep a real taskbar HWND without
       adding a second caption or border around the original client. */
    DWORD style = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName,
        L"Steam (Linux ARM64)", style,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        NULL, NULL, instance, NULL);
    if (!window) return 4;
    // The first Wayland configure can still carry the previous fullscreen size.
    // Keep requested placement until the explicit native client dimensions arrive.
    windowed_bounds=bounds; restore_target=bounds; restore_pending=1;
    SetTimer(window, 1, 150, NULL); ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window); publish(window, 0);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
