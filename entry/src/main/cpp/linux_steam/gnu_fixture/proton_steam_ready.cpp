// Check the real client's local account/IPC session. BLoggedOn describes
// connectivity to Steam's servers, not whether an offline game can be started.
// Game ownership, tickets and DRM remain in the official Steam/Proton APIs.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>

struct SteamLocalSession {
    bool pipe = false, user = false, api = false, localUser = false, online = false;
};

static SteamLocalSession CheckLocalSession(void *client) {
    SteamLocalSession state;
    void **methods = *static_cast<void ***>(client);
    // Public SteamClient017 ABI: CreateSteamPipe, BReleaseSteamPipe,
    // ConnectToGlobalUser, ReleaseUser, GetISteamUser.
    const auto channel = reinterpret_cast<int (*)(void *)>(methods[0])(client);
    state.pipe = channel != 0;
    if (!channel) return state;
    const auto user = reinterpret_cast<int (*)(void *, int)>(methods[2])(client, channel);
    state.user = user != 0;
    void *api = user ? reinterpret_cast<void *(*)(void *, int, int, const char *)>(methods[5])(
        client, user, channel, "SteamUser023") : nullptr;
    state.api = api != nullptr;
    if (api) {
        void **userMethods = *static_cast<void ***>(api);
        state.online = reinterpret_cast<bool (*)(void *)>(userMethods[1])(api);
        const auto id = reinterpret_cast<std::uint64_t (*)(void *)>(userMethods[2])(api);
        // An individual account in a known universe. Never infer identity from
        // loginusers.vdf, a visible library, or a cached launch marker.
        const auto universe = id >> 56;
        state.localUser = (id & UINT64_C(0xffffffff)) != 0 &&
            ((id >> 52) & 15) == 1 && universe > 0 && universe < 5;
    }
    if (user) reinterpret_cast<void (*)(void *, int, int)>(methods[4])(client, channel, user);
    reinterpret_cast<bool (*)(void *, int)>(methods[1])(client, channel);
    return state;
}

int main() {
    const char *root = std::getenv("STEAMOHOS_CODE_ROOT");
    if (!root || std::strcmp(root, "/data/storage/el2/base/files/steamohos-native/proton-probe")) return 2;
    const std::string path = std::string(root) + "/home/.steam/sdkarm64/steamclient.so";
    void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "GNU_STEAM_READY stage=sdk_load error=%s\n", dlerror()); return 3; }
    const auto create = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(library, "CreateInterface"));
    int error = 0;
    void *client = create ? create("SteamClient017", &error) : nullptr;
    if (!client) { std::fprintf(stderr, "GNU_STEAM_READY stage=client_interface error=%d\n", error); return 4; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    unsigned attempts = 0;
    SteamLocalSession state;
    do {
        ++attempts;
        state = CheckLocalSession(client);
        if (state.localUser) {
            std::printf("GNU_STEAM_READY session_ready=1 logged_on=%d attempts=%u\n", state.online, attempts);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "GNU_STEAM_READY session_ready=0 pipe=%d user=%d interface=%d local_user=%d logged_on=%d attempts=%u\n",
        state.pipe, state.user, state.api, state.localUser, state.online, attempts);
    return 5;
}
