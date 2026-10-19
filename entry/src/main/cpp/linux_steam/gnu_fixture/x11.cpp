// Native GNU X11 client: verify the real server's drawable with image readback.
#include <xcb/xcb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = getenv("STEAMOHOS_X11_SOCKET");
    sockaddr_un address{};
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address.sun_path)) return 2;
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        if (fd >= 0) close(fd);
        return 3;
    }
    xcb_connection_t* connection = xcb_connect_to_fd(fd, nullptr);
    if (!connection || xcb_connection_has_error(connection)) return 4;
    const xcb_setup_t* setup = xcb_get_setup(connection);
    xcb_screen_t* screen = xcb_setup_roots_iterator(setup).data;
    if (!screen || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST) {
        xcb_disconnect(connection); return 5;
    }
    xcb_window_t window = xcb_generate_id(connection);
    uint32_t values[] = {screen->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS};
    auto create = xcb_create_window_checked(connection, screen->root_depth, window, screen->root,
        20, 20, 480, 270, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    auto* error = xcb_request_check(connection, create);
    if (error) { printf("X11 create error=%u\n", error->error_code); free(error); xcb_disconnect(connection); return 6; }
    const char title[] = "Native GNU X11 over existing Wayland";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, sizeof(title)-1, title);
    xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, window, 0, nullptr);
    xcb_map_window(connection, window);
    auto* geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
    bool passed = geometry && geometry->width == 480 && geometry->height == 270;
    free(geometry);
    uint32_t frames = 0;
    for (; passed && frames < 18; ++frames) {
        const uint32_t color = frames % 2 ? 0x00cc6633 : 0x003366cc;
        xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &color);
        const xcb_rectangle_t rect{0, 0, 480, 270};
        xcb_poly_fill_rectangle(connection, window, gc, 1, &rect);
        auto* image = xcb_get_image_reply(connection,
            xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, 0, 0, 1, 1, ~0u), &error);
        uint32_t pixel = 0;
        if (image && xcb_get_image_data_length(image) >= 4) memcpy(&pixel, xcb_get_image_data(image), 4);
        passed = image && !error && (pixel & 0x00ffffffu) == color;
        free(error); error = nullptr; free(image);
        xcb_flush(connection);
        usleep(150000);
    }
    xcb_free_gc(connection, gc);
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
    const bool closedCleanly = !xcb_connection_has_error(connection);
    xcb_disconnect(connection);
    printf("STEAMOHOS_GNU_X11_V1 {\"status\":\"%s\",\"framesReadBack\":%u,"
        "\"inputVerified\":false,\"gpuAccelerationVerified\":false}\n",
        passed && closedCleanly ? "x11_drawable_verified" : "x11_failed", frames);
    return passed && closedCleanly ? 0 : 1;
}
