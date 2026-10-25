#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static unsigned paints, ticks, limit = 24, event_count;
static int controls;
static void report_window(HWND window, const char* event, WPARAM value) {
    RECT rect;
    if (!controls || event_count++ >= 200) return;
    GetWindowRect(window, &rect);
    printf("HMOSDECK_WINDOW_CONTROL event=%s value=%llu rect=%ld,%ld,%ld,%ld minimized=%d maximized=%d style=%lx\n",
        event, (unsigned long long)value, rect.left, rect.top, rect.right, rect.bottom,
        IsIconic(window), IsZoomed(window), (unsigned long)GetWindowLongW(window, GWL_STYLE));
    fflush(stdout);
}
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) {
        LRESULT hit = DefWindowProcA(window, message, wparam, lparam);
        if (hit != HTCLIENT) report_window(window, "hit", (WPARAM)hit);
        return hit;
    }
    if (message == WM_NCLBUTTONDOWN || message == WM_SYSCOMMAND || message == WM_MOVE || message == WM_SIZE)
        report_window(window, message == WM_SYSCOMMAND ? "syscommand" :
            message == WM_NCLBUTTONDOWN ? "ncdown" : message == WM_MOVE ? "move" : "size", wparam);
    if (message == WM_TIMER && controls && ticks % 4 == 0) report_window(window, "tick", ticks);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT area;
        GetClientRect(window, &area);
        HBRUSH brush = CreateSolidBrush(RGB(22, 104, 153));
        FillRect(dc, &area, brush);
        DeleteObject(brush);
        area.right /= 2;
        area.bottom /= 2;
        brush = CreateSolidBrush(RGB(239, 179, 47));
        FillRect(dc, &area, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 24, 180, "GNU Wine / FEX x64 / Wayland", 29);
        EndPaint(window, &paint);
        GdiFlush();
        ++paints;
        return 0;
    }
    if (message == WM_TIMER && ++ticks >= limit) {
        KillTimer(window, 1);
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(window, message, wparam, lparam);
}
int main(int argc, char** argv) {
    controls = argc > 1 && !strcmp(argv[1], "controls");
    if (controls) limit = 240;
    WNDCLASSA cls = {0};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "HmosDeckGnuProbe";
    if (!RegisterClassA(&cls)) return 10;
    HWND window = CreateWindowExA(0, cls.lpszClassName, "GNU Proton x64 window probe",
        WS_OVERLAPPEDWINDOW, 64, 64, 640, 400, NULL, NULL, cls.hInstance, NULL);
    if (!window) { fprintf(stderr, "CreateWindow error=%lu\n", GetLastError()); return 11; }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (!SetTimer(window, 1, 500, NULL)) { DestroyWindow(window); return 12; }
    MSG msg;
    int result;
    while ((result = GetMessageA(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (result < 0 || !paints || (!controls && ticks < 24)) return 13;
    printf("HMOSDECK_GNU_X64_WINDOW_OK paints=%u ticks=%u\n", paints, ticks);
    return 0;
}
