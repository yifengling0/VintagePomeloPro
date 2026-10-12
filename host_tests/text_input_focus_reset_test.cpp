// Exercise Proton's real TextInput with the pinned Wayland server and sockets.
#include "input/text_input.h"
#include "protocols/wayland-server-protocol.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static size_t Drain(wl_client* client, int fd) {
    wl_client_flush(client);
    size_t bytes = 0;
    char buffer[4096];
    for (;;) {
        const auto n = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) { bytes += static_cast<size_t>(n); continue; }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        return bytes;
    }
}

int main() {
    auto* text = TextInput::GetInstance();
    std::vector<bool> activation;
    text->SetActivateCallback([&](bool active, int, int, int, int) { activation.push_back(active); });
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto* display = wl_display_create();
        assert(display);
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
        auto* client = wl_client_create(display, sockets[0]);
        assert(client);
        text->Register(display);
        TextInput::manager_bind(client, text, 1, 2);
        auto* manager = wl_client_get_object(client, 2);
        assert(manager);
        TextInput::manager_get_text_input(client, manager, 3, nullptr);
        auto* input = wl_client_get_object(client, 3);
        auto* surface = wl_resource_create(client, &wl_surface_interface, 4, 4);
        assert(input && surface);
        activation.clear();
        auto activate = [&]() {
            text->OnKeyboardEnter(surface);
            TextInput::text_input_enable(client, input);
            TextInput::text_input_set_cursor_rectangle(client, input, 2, 3, 4, 5);
            TextInput::text_input_commit(client, input);
        };
        activate();
        assert(activation == std::vector<bool>{true});
        assert(Drain(client, sockets[1]) > 0);
        text->ResetKeyboardFocus();
        assert((activation == std::vector<bool>{true, false}));
        assert(Drain(client, sockets[1]) == 0); // no leave to a possibly dead surface
        text->ResetKeyboardFocus();
        assert(activation.size() == 2); // no duplicate deactivation
        text->SendCommitString("must not reach the old focus");
        text->SendPreeditString("old", 0, 3);
        text->SendDeleteSurrounding(1, 0);
        assert(Drain(client, sockets[1]) == 0);
        activate();
        assert((activation == std::vector<bool>{true, false, true}));
        assert(Drain(client, sockets[1]) > 0);
        text->SendCommitString("new focus");
        assert(Drain(client, sockets[1]) > 0); // binding survives a local focus reset
        text->ResetKeyboardFocus();
        text->Unregister();
        wl_client_destroy(client);
        close(sockets[1]);
        wl_display_destroy(display);
    }
    text->SetActivateCallback({});
    std::puts("PASS real Wayland IME reset, no stale commits, reacquisition and two engine sessions");
}
