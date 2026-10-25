// Query the real Linux Steam client. A visible window alone does not mean
// that its account/IPC session is ready to authenticate a Proton game.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
int main() {
    const char *root = std::getenv("STEAMOHOS_CODE_ROOT");
    if (!root || std::strcmp(root, "/data/storage/el2/base/files/steamohos-native/proton-probe")) return 2;
    const std::string path = std::string(root) + "/home/.steam/sdkarm64/steamclient.so";
    void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "GNU_STEAM_READY sdk_load_failed=%s\n", dlerror()); return 3; }
    const auto create = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(library, "CreateInterface"));
    int error = 0;
    void *client = create ? create("SteamClient017", &error) : nullptr;
    if (!client) return 4;
    void **methods = *static_cast<void ***>(client);
    // These public SteamClient017 methods retain their original ABI.
    const auto pipe = reinterpret_cast<int (*)(void *)>(methods[0]);
    const auto release = reinterpret_cast<bool (*)(void *, int)>(methods[1]);
    const auto connect = reinterpret_cast<int (*)(void *, int)>(methods[2]);
    const auto userInterface = reinterpret_cast<void *(*)(void *, int, int, const char *)>(methods[5]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    unsigned attempts = 0;
    do {
        ++attempts;
        const int channel = pipe(client);
        const int user = channel ? connect(client, channel) : 0;
        void *api = user ? userInterface(client, user, channel, "SteamUser023") : nullptr;
        bool loggedOn = false;
        if (api) {
            void **userMethods = *static_cast<void ***>(api);
            loggedOn = reinterpret_cast<bool (*)(void *)>(userMethods[1])(api);
        }
        if (channel) release(client, channel);
        if (loggedOn) {
            std::printf("GNU_STEAM_READY authenticated=1 attempts=%u\n", attempts);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "GNU_STEAM_READY authenticated=0 attempts=%u\n", attempts);
    return 5;
}
