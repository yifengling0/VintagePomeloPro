#include "winehua_host.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace {
int Open(void* context,uint32_t timeout) {
    const char* path=static_cast<const char*>(context);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(!path||path[0]!='/'||std::strlen(path)>=sizeof(address.sun_path)){errno=EINVAL;return -1;}
    struct stat st{};
    if(lstat(path,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()){errno=EACCES;return -1;}
    std::strcpy(address.sun_path,path);
    const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
    if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return fd;
    if(errno==EINPROGRESS||errno==EAGAIN) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        while(std::chrono::steady_clock::now()<end) {
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count();
            pollfd p{fd,POLLOUT,0};const auto rc=poll(&p,1,left>0?static_cast<int>(left):0);
            if(rc<0&&errno==EINTR)continue;
            int error=0;socklen_t n=sizeof(error);
            if(rc>0&&(p.revents&POLLOUT)&&!getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)&&!error)return fd;
            break;
        }
    }
    close(fd);return -1;
}
}
extern "C" int steamohos_winehua_probe_graphics_v1(const char* path,SteamOhosGraphicsProbeV1* result,uint32_t timeout) {
    SteamOhosGraphicsHostV1 host{sizeof(host),1,const_cast<char*>(path),Open};
    return steamohos_probe_graphics_host_v1(&host,result,timeout);
}
