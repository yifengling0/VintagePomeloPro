// Exercise the production SteamClient017 / SteamUser023 calls, including
// offline accounts. No Valve libraries, accounts or network are required.
#define main SteamReadinessMain
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/proton_steam_ready.cpp"
#undef main
#include <cassert>

static bool pipeAvailable, userAvailable, apiAvailable, online;
static std::uint64_t identity;
static int releasedUsers, releasedPipes;
struct Object { void **methods; };
static bool IsOnline(void *) { return online; }
static std::uint64_t Identity(void *) { return identity; }
static void *userMethods[] = {nullptr, reinterpret_cast<void *>(IsOnline), reinterpret_cast<void *>(Identity)};
static Object api{userMethods};
static int Pipe(void *) { return pipeAvailable ? 17 : 0; }
static bool ReleasePipe(void *, int pipe) { assert(pipe == 17); ++releasedPipes; return true; }
static int User(void *, int pipe) { assert(pipe == 17); return userAvailable ? 23 : 0; }
static void ReleaseUser(void *, int pipe, int user) { assert(pipe == 17 && user == 23); ++releasedUsers; }
static void *Interface(void *, int user, int pipe, const char *version) {
    assert(user == 23 && pipe == 17 && !std::strcmp(version, "SteamUser023"));
    return apiAvailable ? &api : nullptr;
}
static void *clientMethods[] = {reinterpret_cast<void *>(Pipe), reinterpret_cast<void *>(ReleasePipe),
    reinterpret_cast<void *>(User), nullptr, reinterpret_cast<void *>(ReleaseUser), reinterpret_cast<void *>(Interface)};
static Object client{clientMethods};

static SteamLocalSession Check(bool hasPipe, bool hasUser, bool hasApi, bool connected, std::uint64_t id) {
    pipeAvailable = hasPipe; userAvailable = hasUser; apiAvailable = hasApi; online = connected;
    identity = id; releasedUsers = releasedPipes = 0;
    const auto state = CheckLocalSession(&client);
    assert(releasedPipes == (hasPipe ? 1 : 0));
    assert(releasedUsers == (hasPipe && hasUser ? 1 : 0));
    return state;
}
int main() {
    constexpr auto individual = (UINT64_C(1) << 56) | (UINT64_C(1) << 52) | (UINT64_C(1) << 32) | 42;
    auto state = Check(true, true, true, true, individual);
    assert(state.localUser && state.online);
    state = Check(true, true, true, false, individual);
    assert(state.localUser && !state.online); // offline/reconnecting is allowed
    state = Check(false, true, true, true, individual);
    assert(!state.localUser && !state.pipe);
    state = Check(true, false, true, true, individual);
    assert(!state.localUser && !state.user);
    state = Check(true, true, false, true, individual);
    assert(!state.localUser && !state.api);
    for (auto id : {UINT64_C(0), individual & ~UINT64_C(0xffffffff), individual & ~(UINT64_C(255) << 56),
                   (UINT64_C(5) << 56) | (UINT64_C(1) << 52) | 42,
                   (UINT64_C(1) << 56) | (UINT64_C(7) << 52) | 42}) {
        assert(!Check(true, true, true, true, id).localUser);
        assert(!Check(true, true, true, false, id).localUser);
    }
    std::puts("Steam local readiness: online, offline/reconnect, missing IPC/user/interface, invalid accounts and handle cleanup PASS");
}
