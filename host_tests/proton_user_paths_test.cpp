#include "../entry/src/main/cpp/linux_steam/proton_user_paths.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
int main() {
    char pattern[]="/var/tmp/proton-user-paths-XXXXXX";
    fs::path base=mkdtemp(pattern);assert(!base.empty());
    auto prefix=base/"pfx",user=prefix/"drive_c/users/steamuser";
    assert(steamohos::PrepareProtonUserPaths(prefix));
    assert(steamohos::PrepareProtonUserPaths(prefix));
    fs::create_directories(user/"My Documents/My Games/Test");
    std::ofstream(user/"My Documents/My Games/Test/0.sav")<<"save-bytes";
    struct stat before{},after{};assert(!stat((user/"My Documents/My Games/Test/0.sav").c_str(),&before));
    assert(steamohos::PrepareProtonUserPaths(prefix));
    assert(!stat((user/"Documents/My Games/Test/0.sav").c_str(),&after));
    assert(before.st_ino==after.st_ino&&before.st_size==after.st_size);
    fs::create_directories(user/"My Documents/My Games/Test");
    std::ofstream(user/"My Documents/My Games/Test/0.sav")<<"conflicting";
    std::ofstream(user/"My Documents/unique.sav")<<"unique";
    assert(!steamohos::PrepareProtonUserPaths(prefix)&&errno==EEXIST);
    assert(fs::exists(user/"My Documents/unique.sav")&&!fs::exists(user/"Documents/unique.sav"));
    std::ifstream original(user/"Documents/My Games/Test/0.sav");std::string text;original>>text;assert(text=="save-bytes");
    fs::remove_all(user/"My Documents");
    fs::create_directory_symlink(base,user/"My Documents");
    assert(!steamohos::PrepareProtonUserPaths(prefix)&&errno==EEXIST);
    fs::remove_all(base);
    std::cout<<"PASS modern/XP folder preparation, idempotence, inode preservation, all-conflict preflight and custom-link preservation\n";
}
