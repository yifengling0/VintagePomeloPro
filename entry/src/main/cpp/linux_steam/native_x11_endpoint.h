#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace steamohos {
// One Xwayland, two transports. The native guest uses the kernel loopback
// endpoint; existing host clients keep their private Unix endpoint. Both use
// a fresh per-session MIT cookie. No wildcard-network listener or -ac.
struct NativeX11Endpoint {
    int fd=-1;
    std::string display, authority;
    NativeX11Endpoint()=default;
    NativeX11Endpoint(const NativeX11Endpoint&)=delete;
    NativeX11Endpoint& operator=(const NativeX11Endpoint&)=delete;
    ~NativeX11Endpoint() { if(fd>=0)close(fd);if(!authority.empty())unlink(authority.c_str()); }
    bool Open(const std::string& directory) {
        if(fd>=0 || !authority.empty()){errno=EALREADY;return false;}
        fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        socklen_t size=sizeof(local);
        if(fd<0 || bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local)) ||
           getsockname(fd,reinterpret_cast<sockaddr*>(&local),&size) ||
           ntohs(local.sin_port)<6000 || listen(fd,8))return false;
        const auto number=std::to_string(ntohs(local.sin_port)-6000);
        display="127.0.0.1:"+number;
        unsigned char cookie[16];
        int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if(random<0)return false;
        size_t used=0;
        while(used<sizeof(cookie)) {
            ssize_t count=read(random,cookie+used,sizeof(cookie)-used);
            if(count<0&&errno==EINTR)continue;
            if(count<=0){int saved=count<0?errno:EIO;close(random);errno=saved;return false;}
            used+=static_cast<size_t>(count);
        }
        close(random);
        std::string bytes;
        auto word=[&](unsigned n){bytes+=static_cast<char>(n>>8);bytes+=static_cast<char>(n&255);};
        auto field=[&](const char* data,size_t length){word(length);bytes.append(data,length);};
        // FamilyWild matches local Unix and IPv4 clients. Display numbers
        // remain explicit; the cookie grants access only to this server.
        for(const auto& n:{std::string("57"),number}) {
            word(65535);word(0);field(n.data(),n.size());
            field("MIT-MAGIC-COOKIE-1",18);field(reinterpret_cast<char*>(cookie),sizeof(cookie));
        }
        const auto path=directory+"/xauthority";
        int file=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(file<0)return false;
        authority=path;used=0;int error=0;
        while(used<bytes.size()) {
            ssize_t count=write(file,bytes.data()+used,bytes.size()-used);
            if(count<0&&errno==EINTR)continue;
            if(count<=0){error=count<0?errno:EIO;break;}used+=static_cast<size_t>(count);
        }
        if(close(file)&&!error)error=errno;
        if(error){errno=error;return false;}
        return true;
    }
};
}
