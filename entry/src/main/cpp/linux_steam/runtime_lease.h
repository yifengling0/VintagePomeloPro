#pragma once
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace steamohos {
// One kernel lease across installer, prefix maintenance and the native worker.
// Process death closes the descriptor; no stale PID file or timeout unlock.
class RuntimeLease {
    int fd_=-1;
public:
    RuntimeLease()=default;
    RuntimeLease(const RuntimeLease&)=delete;
    RuntimeLease& operator=(const RuntimeLease&)=delete;
    ~RuntimeLease(){if(fd_>=0)close(fd_);}
    int Descriptor() const {return fd_;}
    bool Adopt(int descriptor,const std::string& base) {
        if(fd_>=0||descriptor<0){errno=EINVAL;return false;}
        int copy=fcntl(descriptor,F_DUPFD_CLOEXEC,3);if(copy<0)return false;
        struct stat file{},path{};
        const int flags=fcntl(copy,F_GETFL);
        if(fstat(copy,&file)||lstat((base+"/.proton-maintenance.lock").c_str(),&path)||
           !S_ISREG(file.st_mode)||file.st_uid!=geteuid()||file.st_nlink!=1||(file.st_mode&077)||
           !S_ISREG(path.st_mode)||file.st_dev!=path.st_dev||file.st_ino!=path.st_ino||
           flags<0||(flags&O_ACCMODE)!=O_RDWR||flock(copy,LOCK_EX|LOCK_NB)) {
            int error=errno;close(copy);errno=error?error:EPERM;return false;
        }
        fd_=copy;return true;
    }
    bool Acquire(const std::string& base) {
        if(fd_>=0){errno=EALREADY;return false;}
        struct stat dir{};
        if(lstat(base.c_str(),&dir)||!S_ISDIR(dir.st_mode)||dir.st_uid!=geteuid()){errno=EPERM;return false;}
        int fd=open((base+"/.proton-maintenance.lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)return false;
        struct stat st{};
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||(st.st_mode&077)) {
            close(fd);errno=EPERM;return false;
        }
        int result;do{result=flock(fd,LOCK_EX|LOCK_NB);}while(result&&errno==EINTR);
        if(result){int error=errno;close(fd);errno=error;return false;}
        fd_=fd;return true;
    }
};
}
