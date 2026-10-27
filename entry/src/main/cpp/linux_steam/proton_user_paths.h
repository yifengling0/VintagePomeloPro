#pragma once
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace steamohos {
// Steam's virtual XP aliases and Wine's modern directories share these bytes.
// Plan all collisions first; never copy or overwrite a user's save file.
inline bool PrepareProtonUserPaths(const std::string& prefix) {
    struct Plan {
        std::vector<std::pair<std::string,std::string>> moves;
        std::vector<std::string> empty;
        size_t entries=0;
        bool Directory(const std::string& path) {
            struct stat st{};
            if(mkdir(path.c_str(),0700)&&errno!=EEXIST)return false;
            if(lstat(path.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()){errno=EPERM;return false;}
            return true;
        }
        bool Merge(const std::string& source,const std::string& destination,unsigned depth=0) {
            if(depth>32||++entries>4096){errno=EOVERFLOW;return false;}
            struct stat old{},current{};
            if(lstat(source.c_str(),&old))return errno==ENOENT;
            if(!S_ISDIR(old.st_mode)||old.st_uid!=getuid()){errno=EEXIST;return false;}
            if(lstat(destination.c_str(),&current)) {
                if(errno!=ENOENT)return false;
                moves.emplace_back(source,destination);return true;
            }
            if(!S_ISDIR(current.st_mode)||current.st_uid!=getuid()){errno=EEXIST;return false;}
            DIR* directory=opendir(source.c_str());if(!directory)return false;
            bool ok=true;
            while(auto* entry=readdir(directory)) {
                if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
                if(++entries>4096){errno=EOVERFLOW;ok=false;break;}
                const std::string from=source+'/'+entry->d_name,to=destination+'/'+entry->d_name;
                struct stat a{},b{};
                if(lstat(from.c_str(),&a)){ok=false;break;}
                if(lstat(to.c_str(),&b)) {
                    if(errno!=ENOENT){ok=false;break;}
                    moves.emplace_back(from,to);
                } else if(S_ISDIR(a.st_mode)&&S_ISDIR(b.st_mode)) {
                    if(!Merge(from,to,depth+1)){ok=false;break;}
                } else {errno=EEXIST;ok=false;break;}
            }
            int error=errno;closedir(directory);errno=error;
            if(ok)empty.push_back(source);
            return ok;
        }
    } plan;
    if(!plan.Directory(prefix))return false;
    int fd=open((prefix+"/.hmosdeck-user-paths.lock").c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return false;
    struct Guard {int fd;~Guard(){close(fd);}} guard{fd};
    struct stat lock{};
    if(fstat(fd,&lock)||!S_ISREG(lock.st_mode)||lock.st_uid!=getuid()||flock(fd,LOCK_EX|LOCK_NB))return false;
    std::string user=prefix+"/drive_c/users/steamuser";
    for(const auto& path:{prefix+"/drive_c",prefix+"/drive_c/users",user,user+"/AppData",user+"/Local Settings"})
        if(!plan.Directory(path))return false;
    const std::pair<const char*,const char*> aliases[]={
        {"My Documents","Documents"},{"Application Data","AppData/Roaming"},
        {"Local Settings/Application Data","AppData/Local"}};
    for(const auto& alias:aliases)if(!plan.Merge(user+'/'+alias.first,user+'/'+alias.second))return false;
    for(const auto& move:plan.moves) {
#ifdef SYS_renameat2
        if(syscall(SYS_renameat2,AT_FDCWD,move.first.c_str(),AT_FDCWD,move.second.c_str(),1))return false;
#else
        errno=ENOSYS;return false;
#endif
    }
    for(const auto& directory:plan.empty)if(rmdir(directory.c_str()))return false;
    for(const auto& alias:aliases)if(!plan.Directory(user+'/'+alias.second))return false;
    return plan.Directory(user+"/AppData/LocalLow")&&plan.Directory(user+"/Saved Games")&&
        plan.Directory(prefix+"/drive_c/ProgramData");
}
}
