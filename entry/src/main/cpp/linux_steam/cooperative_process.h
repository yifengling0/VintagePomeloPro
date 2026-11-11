#pragma once
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace steamohos {
// HSL V5 cooperation: only an authenticated peer may export a read-only,
// unlinked snapshot of its own kernel stat. No fabricated /proc permissions.
inline int BorrowProcessStat(const std::string& root,pid_t pid) {
    struct Channel {int fd=-1,copy=-1;~Channel(){if(fd>=0)close(fd);if(copy>=0)close(copy);}} channel;
    const auto endpoint=root+"/socket-metadata/"+std::to_string(pid);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(pid<=1||endpoint.size()>=sizeof(address.sun_path)){errno=EINVAL;return -1;}
    strcpy(address.sun_path,endpoint.c_str());
    channel.fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(channel.fd<0)return -1;
    if(connect(channel.fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))&&errno!=EINPROGRESS)return -1;
    using Clock=std::chrono::steady_clock;const auto deadline=Clock::now()+std::chrono::milliseconds(50);
    auto ready=[&](short events) {
        for(;;) {
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(remaining<=0){errno=ETIMEDOUT;return false;}
            pollfd event{channel.fd,events,0};int result=poll(&event,1,static_cast<int>(remaining));
            if(result<0&&errno==EINTR)continue;
            if(result<=0){if(!result)errno=ETIMEDOUT;return false;}return true;
        }
    };
    if(!ready(POLLOUT))return -1;
    ucred peer{};socklen_t size=sizeof(peer);
    if(getsockopt(channel.fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)||size!=sizeof(peer)||peer.pid!=pid||peer.uid!=geteuid()){errno=EACCES;return -1;}
    struct {uint32_t version;int32_t descriptor;uint64_t inode;} request{5,0,0};
    size_t used=0;
    while(used<sizeof(request)) {
        if(!ready(POLLOUT))return -1;
        const auto n=send(channel.fd,reinterpret_cast<char*>(&request)+used,sizeof(request)-used,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<=0)return -1;
        used+=n;
    }
    struct {int32_t error;uint32_t reserved;uint64_t inode;} reply{};
    used=0;unsigned rights=0;
    while(used<sizeof(reply)) {
        if(!ready(POLLIN))return -1;
        iovec io{reinterpret_cast<char*>(&reply)+used,sizeof(reply)-used};
        union {cmsghdr aligned;char bytes[CMSG_SPACE(4*sizeof(int))];} ancillary{};
        msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
        const auto n=recvmsg(channel.fd,&message,MSG_CMSG_CLOEXEC|MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        for(auto* header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header)) {
            if(header->cmsg_level!=SOL_SOCKET||header->cmsg_type!=SCM_RIGHTS||header->cmsg_len<CMSG_LEN(0))continue;
            for(size_t offset=0;offset+sizeof(int)<=header->cmsg_len-CMSG_LEN(0);offset+=sizeof(int)) {
                int fd;memcpy(&fd,reinterpret_cast<char*>(CMSG_DATA(header))+offset,sizeof(fd));
                if(!rights++)channel.copy=fd;else close(fd);
            }
        }
        if(n<=0||rights>1||(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))){errno=n<0?errno:EPROTO;return -1;}
        used+=n;
    }
    if(reply.error){errno=reply.error;return -1;}
    struct stat status{};const int flags=fcntl(channel.copy,F_GETFL);
    if(rights!=1||reply.reserved||!reply.inode||flags<0||(flags&O_ACCMODE)!=O_RDONLY||fstat(channel.copy,&status)||
       !S_ISREG(status.st_mode)||status.st_uid!=geteuid()||status.st_nlink||status.st_size<=0||status.st_size>=4096){errno=EPROTO;return -1;}
    const int result=channel.copy;channel.copy=-1;return result;
}
}
