// Exercise the same standard DISPLAY / Xlib lookup used by unmodified applications.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdio>
#include <clocale>
#include <unistd.h>
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Display* display = XOpenDisplay(nullptr);
    if (!display) { puts("STEAMOHOS_GNU_XLIB_V1 open_failed"); return 2; }
    // Exercise the title conversion used by original VGUI before starting it.
    // Missing XLC data otherwise leaves a caller's output record uninitialized.
    wchar_t title[] = L"Native GNU standard Xlib DISPLAY";
    wchar_t* titles[] = {title};
    XTextProperty property{};
    const int converted = XwcTextListToTextProperty(display, titles, 1, XUTF8StringStyle, &property);
    if (converted != Success || !property.value) {
        printf("STEAMOHOS_GNU_XLIB_V1 locale_failed result=%d supported=%d\n", converted, XSupportsLocale());
        if (property.value) XFree(property.value);
        XCloseDisplay(display); return 3;
    }
    puts("STEAMOHOS_GNU_XLIB_V1 xlib_locale_verified");
    const int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 20, 20, 480, 270,
                                         0, 0, 0);
    XSetWMName(display, window, &property);
    XFree(property.value);
    XMapWindow(display, window);
    GC gc = XCreateGC(display, window, 0, nullptr);
    unsigned frames = 0;
    bool verified = true;
    for (; verified && frames < 18; ++frames) {
        const unsigned long color = frames % 2 ? 0x0022aa66 : 0x006633cc;
        XSetForeground(display, gc, color);
        XFillRectangle(display, window, gc, 0, 0, 480, 270);
        XSync(display, False);
        XImage* image = XGetImage(display, window, 0, 0, 1, 1, AllPlanes, ZPixmap);
        verified = image && (XGetPixel(image, 0, 0) & 0xffffffu) == color;
        if (image) XDestroyImage(image);
        usleep(150000);
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    printf("STEAMOHOS_GNU_XLIB_V1 {\"status\":\"%s\",\"framesReadBack\":%u}\n",
           verified ? "xlib_display_verified" : "xlib_failed", frames);
    return verified ? 0 : 1;
}
