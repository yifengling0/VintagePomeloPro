#include <cassert>
#include <iostream>
#include "../entry/src/main/cpp/linux_steam/proton_game_paths.h"
int main() {
    using steamohos::ProtonGameWindowsPath;
    const std::string root="/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/Any Game";
    assert(ProtonGameWindowsPath(root+"/bin/Game.exe",root)=="J:\\bin\\Game.exe");
    assert(ProtonGameWindowsPath(root,root)=="J:\\");
    assert(ProtonGameWindowsPath(root+"/data/中文.dat",root)=="J:\\data\\中文.dat");
    assert(ProtonGameWindowsPath(root+"-other/file",root)=="Z:\\storage\\Users\\currentUser\\Download\\com.vintage.pomelopro\\SteamLinuxARM64\\library\\steamapps\\common\\Any Game-other\\file");
    assert(ProtonGameWindowsPath("/client/helper.exe",root)=="Z:\\client\\helper.exe");
    assert(ProtonGameWindowsPath("/client/helper.exe","")=="Z:\\client\\helper.exe");
    std::cout << "PASS: scoped game drive, nested paths, Unicode, sibling isolation and client Z: paths\n";
}
