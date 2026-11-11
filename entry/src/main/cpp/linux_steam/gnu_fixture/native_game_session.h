#pragma once
#include "../launch_wire.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace steamohos {
// A wrapper observes its own child. Host controls never contain a PID and can
// affect only this launch's process group. Steam/FEXServer are outside it.
class NativeGameSession {
    int fd=-1;
    static uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); }
    static std::string Env(const char* key) { const char* value=getenv(key);return value?value:""; }
    bool Transfer(void* bytes,size_t size,bool sending) {
        if(fd<0)return false;
        auto* data=static_cast<char*>(bytes);const auto deadline=Now()+30000;
        while(size&&Now()<deadline) {
            pollfd event{fd,static_cast<short>(sending?POLLOUT:POLLIN),0};
            const int ready=poll(&event,1,100);
            if(ready<0&&errno==EINTR)continue;
            if(ready<0)return false;
            if(!ready)continue;
            const auto n=sending?send(fd,data,size,MSG_NOSIGNAL):recv(fd,data,size,0);
            if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
            if(n<=0)return false;
            data+=n;size-=n;
        }
        return size==0;
    }
    bool Event(uint32_t kind,uint32_t value,uint32_t signal) {
        uint32_t data[]={htonl(NativeEventMagic),htonl(kind),htonl(value),htonl(signal)};
        return Transfer(data,sizeof(data),true);
    }
public:
    ~NativeGameSession() { if(fd>=0)close(fd); }
    bool Start(const std::string& game,const std::string& install) {
        const auto endpoint=Env("STEAMOHOS_LAUNCH_SOCKET"),nonce=Env("STEAMOHOS_LAUNCH_NONCE");
        if(endpoint.empty()||endpoint[0]!='/'||endpoint.size()>=sizeof(sockaddr_un::sun_path)||nonce.size()!=64)return false;
        LaunchRequest request;request.backend="linux-native";request.verb="observe";request.nonce=nonce;
        request.appId=Env("SteamAppId");if(request.appId.empty())request.appId=Env("STEAM_COMPAT_APP_ID");
        request.cwd=install;request.install=install;request.argv={game};
        sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,endpoint.c_str());
        fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return false;
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))&&errno!=EINPROGRESS)return false;
        auto frame=EncodeLaunch(request);uint32_t reply[3]{};
        if(!Transfer(frame.data(),frame.size(),true)||!Transfer(reply,sizeof(reply),false))return false;
        return ntohl(reply[0])==LaunchReplyMagic&&ntohl(reply[1])==1;
    }
    // No fd survives exec into the game; cancellation is owned by this wrapper.
    bool Started(pid_t pid) { return Event(1,static_cast<uint32_t>(pid),0); }
    int Wait(pid_t child,int& status) {
        std::string input;uint64_t stopping=0;
        for(;;) {
            const pid_t result=waitpid(child,&status,WNOHANG);
            if(result==child) {
                const unsigned code=WIFEXITED(status)?WEXITSTATUS(status):0;
                Event(2,code,WIFSIGNALED(status)?WTERMSIG(status):0);return 0;
            }
            if(result<0&&errno!=EINTR)return -1;
            if(stopping&&Now()-stopping>5000) { kill(-child,SIGKILL);stopping=Now(); }
            pollfd event{fd,POLLIN,0};const int ready=poll(&event,1,50);
            if(ready<0&&errno==EINTR)continue;
            if(ready<0||!ready)continue;
            char bytes[48];const auto n=recv(fd,bytes,sizeof(bytes),0);
            if(n>0)input.append(bytes,static_cast<size_t>(n));
            else if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
            else { if(!stopping){kill(-child,SIGTERM);stopping=Now();}close(fd);fd=-1;continue; }
            if(input.size()>48) {if(!stopping){kill(-child,SIGTERM);stopping=Now();}input.clear();}
            while(input.size()>=12) {
                uint32_t reply[3];memcpy(reply,input.data(),12);input.erase(0,12);
                const auto magic=ntohl(reply[0]),kind=ntohl(reply[1]);
                if(magic!=LaunchReplyMagic||kind==6) {kill(-child,SIGKILL);stopping=Now();}
                else if(kind==5||kind==3||kind==4) {kill(-child,SIGTERM);if(!stopping)stopping=Now();}
            }
        }
    }
};
}
