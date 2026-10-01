#include "wine/steam_client_args.h"
#include <cassert>
#include <cstdio>

int main()
{
    char wine[] = "wine", steam[] = "C:\\Program Files (x86)\\Steam\\steam.exe";
    char launch[] = "-applaunch", app[] = "235900", gameFlag[] = "--game-option";
    char sandbox[] = "-no-cef-sandbox", gpu[] = "-cef-force-gpu";
    char* argv[9] = {wine, steam, launch, app, gameFlag, sandbox, nullptr};
    int argc = 6;
    // A game argument with the same spelling does not configure the client.
    assert(!winehua::HasSteamClientArg(argc, argv, sandbox));
    assert(winehua::InsertSteamClientArg(argc, argv, 9, sandbox));
    assert(winehua::InsertSteamClientArg(argc, argv, 9, gpu));
    assert(argc == 8 && argv[2] == sandbox && argv[3] == gpu && argv[4] == launch);
    assert(argv[5] == app && argv[6] == gameFlag && argv[7] == sandbox && !argv[8]);
    assert(winehua::HasSteamClientArg(argc, argv, sandbox));
    assert(!winehua::InsertSteamClientArg(argc, argv, 9, gpu));
    assert(argc == 8 && !argv[8]);
    // Opening Steam without -applaunch keeps its existing argument order.
    char* plain[4] = {wine, steam, nullptr};
    int plainCount = 2;
    assert(winehua::InsertSteamClientArg(plainCount, plain, 4, sandbox));
    assert(plainCount == 3 && plain[2] == sandbox && !plain[3]);
    std::puts("Steam client arguments: PASS");
}
