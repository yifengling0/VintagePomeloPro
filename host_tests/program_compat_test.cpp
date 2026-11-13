#include "proc/program_compat.h"
#include "wine/steam_client_args.h"
#include <cassert>
#include <cstdio>
int main() {
    char game[] = "Z:\\games\\Example App\\Example App.exe";
    char quoted[] = "\"C:\\Example App.exe\"";
    char wine[] = "/wine/bin/wine", bare[] = "EXAMPLE APP.EXE";
    char steam[] = ".\\steam.exe", cmd[] = "cmd.exe", doc[] = "explorer.exe";
    char sandbox[] = "-no-cef-sandbox", launch[] = "-applaunch", id[] = "12345";
    for (char* image : {game, quoted, bare}) {
        char* direct[] = {image, nullptr};
        char* loaded[] = {wine, image, nullptr};
        assert(winehua::ProcessImageName(1, direct) == "example app.exe");
        assert(winehua::ProcessImageName(2, loaded) == "example app.exe");
    }
    for (char* image : {steam, cmd, doc}) {
        char* args[] = {image, game, nullptr};
        assert(winehua::ProcessImageName(2, args) != "example app.exe");
    }
    assert(winehua::ProcessImageName(0, nullptr).empty());
    char* direct[6] = {steam, launch, id, nullptr}; int n = 3;
    assert(winehua::ProcessImageName(n, direct) == "steam.exe");
    assert(winehua::InsertSteamClientArg(n, direct, 6, sandbox));
    assert(direct[1] == sandbox && direct[2] == launch && direct[3] == id && !direct[4]);
    std::puts("process image and direct Steam arguments PASS");
}
