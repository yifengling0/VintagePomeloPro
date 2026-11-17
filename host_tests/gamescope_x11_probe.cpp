// SPDX-License-Identifier: LGPL-2.1-or-later
// Bounded pixel source, staged only in the Gamescope debug candidate. This
// tests X11 -> SHM -> compositor output, never GPU-game interoperability.
#include <AbilityKit/native_child_process.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <chrono>
#include <cstring>
#include <thread>
extern "C" __attribute__((visibility("default"))) void Main(NativeChildProcess_Args args) {
    const char* name = args.entryParams;
    if (!name || name[0] != ':' || strlen(name) < 2 || strlen(name) > 7 ||
        strspn(name + 1, "0123456789") != strlen(name + 1)) return;
    Display* display = XOpenDisplay(name);
    if (!display) return;
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0,
        800, 600, 0, BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "VPP Gamescope pixel probe");
    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(display, window, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&fullscreen), 1);
    XMapWindow(display, window);
    GC gc = XCreateGC(display, window, 0, nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (unsigned frame = 0; std::chrono::steady_clock::now() < deadline; ++frame) {
        while (XPending(display)) { XEvent event; XNextEvent(display, &event); }
        const unsigned long colors[]{0xff2020, 0x20ff20, 0x2020ff, 0xffff20};
        for (unsigned i = 0; i < 4; ++i) {
            XSetForeground(display, gc, colors[(i + frame/30) % 4]);
            XFillRectangle(display, window, gc, (i%2)*400, (i/2)*300, 400, 300);
        }
        XFlush(display);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    XFreeGC(display, gc); XDestroyWindow(display, window); XCloseDisplay(display);
}
