#include "input/text_input.h"
#include "protocols/text-input-unstable-v3-server-protocol.h"
#include "protocols/wayland-server-protocol.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// The boundary is the real pinned Wayland server, including its resource
// destructors, event-loop FD ownership and protocol event marshalling.
static int failAllocation = 0;
static unsigned registrations = 0;
extern "C" {
int __real_pipe2(int[2], int);
wl_global* __real_wl_global_create(wl_display*, const wl_interface*, int, void*, wl_global_bind_func_t);
wl_event_source* __real_wl_event_loop_add_fd(wl_event_loop*, int, uint32_t,
                                            wl_event_loop_fd_func_t, void*);
int __wrap_pipe2(int fds[2], int flags) {
    if (failAllocation == 1) { errno = EMFILE; return -1; }
    return __real_pipe2(fds, flags);
}
wl_global* __wrap_wl_global_create(wl_display* display, const wl_interface* interface,
                                  int version, void* data, wl_global_bind_func_t bind) {
    if (failAllocation == 3) return nullptr;
    wl_global* result = __real_wl_global_create(display, interface, version, data, bind);
    if (result) ++registrations;
    return result;
}
wl_event_source* __wrap_wl_event_loop_add_fd(wl_event_loop* loop, int fd, uint32_t mask,
                                            wl_event_loop_fd_func_t callback, void* data) {
    if (failAllocation == 2) return nullptr;
    return __real_wl_event_loop_add_fd(loop, fd, mask, callback, data);
}
}

static unsigned checks = 0;
static void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static size_t OpenFds() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator());
}
struct Events {
    std::vector<std::string> commits;
    std::vector<std::string> preedits;
    unsigned enters = 0;
    unsigned deletes = 0;
    unsigned done = 0;
};
static void ProtocolEvent(void* data, wl_protocol_logger_type direction,
                           const wl_protocol_logger_message* message) {
    if (direction != WL_PROTOCOL_LOGGER_EVENT ||
        std::strcmp(wl_resource_get_class(message->resource), "zwp_text_input_v3")) return;
    auto& events = *static_cast<Events*>(data);
    const std::string name = message->message->name;
    if (name == "commit_string") events.commits.emplace_back(message->arguments[0].s);
    if (name == "preedit_string") events.preedits.emplace_back(message->arguments[0].s);
    if (name == "enter") ++events.enters;
    if (name == "delete_surrounding_text") ++events.deletes;
    if (name == "done") ++events.done;
}
static void Drain(wl_display* display) {
    Check(wl_event_loop_dispatch(wl_display_get_event_loop(display), 0) == 0,
          "Wayland event dispatch failed");
}

int main() {
    try {
        auto* manager = TextInputManager::GetInstance();
        const size_t initialFds = OpenFds();
        unsigned activated = 0, deactivated = 0;
        manager->SetActivateCallback([&](bool active, int, int, int, int) {
            // Callback must run outside manager locks.
            (void)manager->IsEnabled();
            active ? ++activated : ++deactivated;
        });
        for (int cycle = 0; cycle < 3; ++cycle) {
            wl_display* display = wl_display_create();
            Check(display != nullptr, "display creation failed");
            const unsigned before = registrations;
            manager->Register(display);
            Check(registrations == before + 1, "new display did not register text input");
            manager->Register(display);
            Check(registrations == before + 1, "duplicate global registered");

            Events events;
            auto* logger = wl_display_add_protocol_logger(display, ProtocolEvent, &events);
            Check(logger != nullptr, "protocol logger creation failed");
            int sockets[2];
            Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                  "client socketpair failed");
            wl_client* client = wl_client_create(display, sockets[0]);
            Check(client != nullptr, "client creation failed");
            TextInputManager::manager_bind(client, manager, 1, 2);
            wl_resource* binding = wl_client_get_object(client, 2);
            wl_resource* surface = wl_resource_create(client, &wl_surface_interface, 1, 3);
            TextInputManager::manager_get_text_input(client, binding, 4, nullptr);
            wl_resource* input = wl_client_get_object(client, 4);
            Check(input && surface, "protocol resources missing");
            manager->OnKeyboardEnter(1, surface);
            Check(events.enters == 0, "armed state leaked into new display");
            manager->SetArmed(true);
            Drain(display);
            Check(events.enters == 1, "new client did not receive text-input enter");
            TextInputManager::ti_enable(client, input);
            TextInputManager::ti_set_cursor_rectangle(client, input, 2, 3, 4, 5);
            TextInputManager::ti_commit(client, input);
            Check(manager->IsEnabled(), "text input did not enable");
            Check(manager->SendPreedit("中文预编辑", 0, 5), "preedit enqueue failed");
            Check(manager->SendCommit("重启后中文"), "commit enqueue failed");
            Check(manager->SendDeleteSurrounding(1, 0), "delete enqueue failed");
            Drain(display);
            Check(events.commits == std::vector<std::string>{"重启后中文"}, "Chinese commit not sent");
            Check(events.preedits == std::vector<std::string>{"中文预编辑"}, "preedit not sent");
            Check(events.done == 2 && events.deletes == 1, "input transaction incomplete");

            Check(manager->SendCommit("retired-resource"), "old resource enqueue failed");
            wl_resource_destroy(input);
            TextInputManager::manager_get_text_input(client, binding, 5, nullptr);
            input = wl_client_get_object(client, 5);
            TextInputManager::ti_enable(client, input);
            Drain(display);
            Check(events.commits.size() == 1, "retired resource delivered queued text");

            manager->OnSurfaceDestroyed(surface);
            wl_resource_destroy(surface);
            Check(!manager->IsEnabled() && !manager->SendCommit("dead-focus"), "dead surface retained focus");
            surface = wl_resource_create(client, &wl_surface_interface, 1, 6);
            manager->OnKeyboardEnter(2, surface);
            TextInputManager::ti_enable(client, input);
            TextInputManager::ti_set_cursor_rectangle(client, input, 2, 3, 4, 5);
            Check(manager->SendCommit("retired-session"), "pending stop input missing");

            // NAPI is allowed to submit while the (already joined) Wayland
            // thread is being cleaned up. No writes may use retired pipe FDs.
            std::atomic<bool> began{false};
            std::thread producer([&] {
                began = true;
                for (int i = 0; i < 1000; ++i) manager->SendCommit("pending-producer");
            });
            while (!began) std::this_thread::yield();
            manager->Shutdown();
            producer.join();
            Check(!manager->IsEnabled() && !manager->SendCommit("stopped"), "stopped manager accepts text");
            Check(wl_client_get_object(client, 5) == nullptr, "text-input resource not destroyed");
            manager->Shutdown();
            manager->SetArmed(true); // A late old-UI request must not arm the next display.
            Check(events.commits.size() == 1, "shutdown flushed old text");
            wl_display_destroy_clients(display);
            wl_protocol_logger_destroy(logger);
            wl_display_destroy(display);
            close(sockets[1]);
            Check(OpenFds() == initialFds, "display restart leaked descriptors");
        }
        Check(activated == 6 && deactivated == 3, "activation teardown callback missing or repeated");
        manager->SetActivateCallback(nullptr);
        // Roll back each partial registration instead of publishing a manager
        // that accepts input without an event source. A later retry must work.
        for (int failure = 1; failure <= 3; ++failure) {
            wl_display* display = wl_display_create();
            const size_t displayFds = OpenFds();
            const unsigned before = registrations;
            failAllocation = failure;
            manager->Register(display);
            failAllocation = 0;
            Check(registrations == before && OpenFds() == displayFds, "failed registration leaked resources");
            manager->Register(display);
            Check(registrations == before + 1, "registration retry failed");
            manager->Shutdown();
            wl_display_destroy_clients(display);
            wl_display_destroy(display);
            Check(OpenFds() == initialFds, "registration rollback leaked descriptors");
        }
        std::cout << "TextInput real-Wayland lifecycle: " << checks << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "TextInput lifecycle FAIL: " << error.what() << '\n';
        return 1;
    }
}
