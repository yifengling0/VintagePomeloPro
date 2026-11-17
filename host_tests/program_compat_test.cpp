#include "proc/program_compat.h"
#include "wine/steam_client_args.h"
#include "wine/steam_webhelper_compat.h"
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
    char cef[] = "Z:\\Steam\\bin\\cef\\cef.win7x64\\STEAMWEBHELPER.EXE";
    char* helper[] = {wine, cef, nullptr};
    const std::string rules = "ddraw=b;d3d9=n;d3d11,dxgi=n;vulkan-1=b";
    std::string result = "unchanged";
    auto apply = [&](int count, char* const args[], const char* backend, const char* overlay,
                     const std::string& input) {
        return winehua::SteamWebhelperD3d9Overrides(count, args, backend, overlay, input, result);
    };
    assert(apply(2, helper, "dxvk_legacy_d3d9", "1", rules));
    assert(result == "ddraw=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b");
    assert(winehua::NativeDllPreferred(result, "d3d11"));
    assert(winehua::NativeDllPreferred(result, "dxgi"));
    assert(!apply(2, helper, "dxvk_legacy_d3d9", "0", result));
    assert(!apply(2, helper, "dxvk_legacy_d3d9", "1", result));
    for (const char* backend : {"dxvk_legacy", "dxvk_modern", "wined3d", "wined3d_zink"})
        assert(!apply(2, helper, backend, "1", rules));
    for (char* image : {game, steam, cmd, doc}) {
        char* args[] = {wine, image, cef, nullptr};
        result = "unchanged";
        assert(!apply(3, args, "dxvk_legacy_d3d9", "1", rules));
        assert(result == "unchanged");
    }
    assert(!apply(2, helper, "dxvk_legacy_d3d9", "1", "d3d9=b"));
    assert(!apply(2, helper, "dxvk_legacy_d3d9", "1", "broken"));
    assert(!apply(0, nullptr, "dxvk_legacy_d3d9", "1", rules));
    assert(apply(2, helper, "dxvk_legacy_d3d9", "1", "*d3d9=n;dxgi=n"));
    assert(!winehua::NativeDllPreferred(result, "d3d9"));
    std::puts("process image, Steam arguments and scoped CEF D3D9 compatibility PASS");
}
