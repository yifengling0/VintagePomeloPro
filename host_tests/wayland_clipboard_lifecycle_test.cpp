// Exercise the production protocol/resource callbacks and event-loop transfers.
#include "input/data_device.cpp"
#include <cassert>
#include <iostream>
#include <sys/socket.h>

static void Dispatch() { assert(wl_event_loop_dispatch(wl_display_get_event_loop(display), 0) >= 0); }
static std::string Snapshot(uint64_t* v = nullptr) { char* text = WineClipboardSnapshot(v); std::string out(text); free(text); return out; }
static void ReadPipe(const std::string& text) {
    int p[2]; assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], text.data(), text.size()) == static_cast<ssize_t>(text.size())); close(p[1]); Dispatch();
}
int main() {
    wl_display* server = wl_display_create(); assert(server); WineClipboardRegister(server);
    int a[2], b[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, a));
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, b));
    auto* clientA = wl_client_create(server, a[0]); auto* clientB = wl_client_create(server, b[0]); assert(clientA && clientB);
    auto* surfaceA = wl_resource_create(clientA, &wl_surface_interface, 1, 0);
    auto* surfaceB = wl_resource_create(clientB, &wl_surface_interface, 1, 0);
    auto* seatA = wl_resource_create(clientA, &wl_seat_interface, 2, 0);
    auto* seatB = wl_resource_create(clientB, &wl_seat_interface, 2, 0);
    Bind(clientA, nullptr, 2, 0); auto* managerA = managers.back(); GetDevice(clientA, managerA, 0, seatA);
    Bind(clientB, nullptr, 2, 0); auto* managerB = managers.back(); GetDevice(clientB, managerB, 0, seatB);
    WineClipboardFocus(surfaceA); WineClipboardPost("host"); Dispatch();
    assert(!offers.empty()); for (auto* offer : offers) assert(wl_resource_get_client(offer->resource) == clientA);
    WineClipboardFocus(surfaceB); assert(wl_resource_get_client(offers.back()->resource) == clientB);
    // Host empty text remains a valid offer, and old offers retain their snapshot.
    auto* old = offers.front(); WineClipboardPost(""); Dispatch(); assert(*old->text == "host"); assert(offers.back()->text->empty());
    // Unsupported replacement must not expose an old offer to a newly focused client.
    CreateSource(clientB, managerB, 0); auto* unsupported = sources.rbegin()->first;
    SetSelection(clientB, devices.back(), unsupported, 1); assert(!hasSelection && !selectionText);
    wl_resource_destroy(unsupported);
    ReadPipe("guest"); assert(Snapshot() == "guest"); ReadPipe(""); assert(Snapshot().empty());
    int p[2]; assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], "partial", 7) == 7); Dispatch(); assert(transfers.size() == 1);
    auto* expired = transfers.front(); Timeout(expired); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    assert(write(p[1], "stale", 5) == 5); WineClipboardReset(); Dispatch(); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    // Four stalled writes occupy the shared read/write budget; fifth is closed.
    std::vector<int> readers;
    for (int i = 0; i < 4; ++i) { assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); readers.push_back(p[0]); assert(StartTransfer(p[1], true, std::string(MaxText, 'x'))); }
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(!StartTransfer(p[1], true, "fifth")); assert(fcntl(p[1], F_GETFD) == -1); close(p[0]);
    Invalidate(); assert(transfers.empty()); for (int fd : readers) close(fd);
    // A full text is allowed; one byte over the cap never replaces the cache.
    assert(!pipe2(p, O_CLOEXEC | O_NONBLOCK)); assert(StartTransfer(p[0], false));
    transfers.back()->text.assign(MaxText, 'x'); assert(write(p[1], "x", 1) == 1); Dispatch(); close(p[1]); assert(transfers.empty()); assert(Snapshot().empty());
    WineClipboardFocus(surfaceA); wl_resource_destroy(surfaceA); assert(focusClient == nullptr);
    WineClipboardUnregister(); assert(offers.empty() && sources.empty() && devices.empty() && managers.empty() && transfers.empty());
    wl_client_destroy(clientA); wl_client_destroy(clientB); close(a[1]); close(b[1]); wl_display_destroy(server);
    server = wl_display_create(); WineClipboardRegister(server); assert(Snapshot().empty()); WineClipboardUnregister(); wl_display_destroy(server);
    std::cout << "PASS production Wayland clipboard: focus clients, immutable/empty offers, EOF, timeout, cancel/reset, limits and restart\n";
}
