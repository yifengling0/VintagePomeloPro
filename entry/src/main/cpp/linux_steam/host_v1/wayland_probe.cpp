#include "host.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
struct Fd { int value;~Fd(){if(value>=0)close(value);} };
uint32_t Word(const unsigned char* p) { uint32_t v;std::memcpy(&v,p,4);return v; }
int Ready(int fd,short event,Clock::time_point end) {
    for(;;) {
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(end-Clock::now()).count();
        if(remaining<=0)return STEAMOHOS_GRAPHICS_TIMEOUT;
        pollfd p{fd,event,0};const auto rc=poll(&p,1,static_cast<int>(remaining));
        if(rc<0&&errno==EINTR)continue;
        if(rc==0)return STEAMOHOS_GRAPHICS_TIMEOUT;
        if(rc<0)return STEAMOHOS_GRAPHICS_TRANSPORT;
        if(p.revents&event)return STEAMOHOS_GRAPHICS_VERIFIED;
        return STEAMOHOS_GRAPHICS_TRANSPORT;
    }
}
int Interface(const unsigned char* s,size_t bytes) {
    const char* names[]={"wl_compositor","wl_shm","xdg_wm_base","wl_seat","wl_output",
                         "wl_subcompositor","wp_viewporter","xwayland_shell_v1"};
    for(int i=0;i<STEAMOHOS_WL_GLOBAL_COUNT;++i)
        if(std::strlen(names[i])+1==bytes&&!std::memcmp(s,names[i],bytes))return i;
    return -1;
}
struct Global { uint32_t id,version;int kind; };
// The OHOS sysroot's CMSG_NXTHDR macro compares size_t with a pointer difference.
// Keep the SDK macro and limit its Clang warning workaround to this expansion.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
cmsghdr* NextControl(msghdr* message,cmsghdr* current) { return CMSG_NXTHDR(message,current); }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
int Probe(int fd,SteamOhosGraphicsProbeV1& result,Clock::time_point end) {
    int type=0;socklen_t length=sizeof(type);sockaddr_storage address{};socklen_t alen=sizeof(address);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length)||type!=SOCK_STREAM
        ||getsockname(fd,reinterpret_cast<sockaddr*>(&address),&alen)||address.ss_family!=AF_UNIX)
        return STEAMOHOS_GRAPHICS_TRANSPORT;
    // Fresh connection: wl_display.get_registry(new_id=2), then sync(new_id=3).
    const uint32_t requests[]={1,(12u<<16)|1,2,1,12u<<16,3};
    size_t sent=0;
    while(sent<sizeof(requests)) {
        const auto state=Ready(fd,POLLOUT,end);if(state)return state;
        auto n=send(fd,reinterpret_cast<const char*>(requests)+sent,sizeof(requests)-sent,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
        if(n<=0)return STEAMOHOS_GRAPHICS_TRANSPORT;
        sent+=size_t(n);
    }
    std::array<unsigned char,8192> pending{};size_t used=0,total=0;
    std::vector<Global> globals;
    while(Clock::now()<end) {
        size_t at=0;
        while(used-at>=8) {
            const auto* p=pending.data()+at;
            const uint32_t object=Word(p),header=Word(p+4),size=header>>16,op=header&65535;
            if(size<8||size>4096||size%4)return STEAMOHOS_GRAPHICS_PROTOCOL;
            if(size>used-at)break;
            if(object==2&&op==0) { // wl_registry.global
                if(size<24||globals.size()>=128||result.globals_seen>=256)return STEAMOHOS_GRAPHICS_PROTOCOL;
                const uint32_t id=Word(p+8),bytes=Word(p+12);
                if(!id||bytes<2||bytes>256||size!=20+((bytes+3)&~3u)
                    ||p[16+bytes-1]!=0||std::memchr(p+16,0,bytes-1))return STEAMOHOS_GRAPHICS_PROTOCOL;
                for(const auto& g:globals)if(g.id==id)return STEAMOHOS_GRAPHICS_PROTOCOL;
                const uint32_t version=Word(p+size-4);if(!version)return STEAMOHOS_GRAPHICS_PROTOCOL;
                globals.push_back({id,version,Interface(p+16,bytes)});++result.globals_seen;
            } else if(object==2&&op==1&&size==12) { // wl_registry.global_remove
                const auto id=Word(p+8);bool found=false;
                for(auto i=globals.begin();i!=globals.end();++i)if(i->id==id){globals.erase(i);found=true;break;}
                if(!found)return STEAMOHOS_GRAPHICS_PROTOCOL;
            } else if(object==3&&op==0&&size==12) {
                for(const auto& g:globals)if(g.kind>=0&&result.versions[g.kind]<g.version)result.versions[g.kind]=g.version;
                result.sync_received=1;
                return result.versions[STEAMOHOS_WL_COMPOSITOR]>=4&&result.versions[STEAMOHOS_WL_SHM]>=1
                    &&result.versions[STEAMOHOS_XDG_WM_BASE]>=1&&result.versions[STEAMOHOS_WL_SEAT]>=3
                    ? STEAMOHOS_GRAPHICS_VERIFIED:STEAMOHOS_GRAPHICS_MISSING_GLOBALS;
            } else return STEAMOHOS_GRAPHICS_PROTOCOL; // Includes wl_display.error.
            at+=size;
        }
        if(at){std::memmove(pending.data(),pending.data()+at,used-at);used-=at;}
        if(used==pending.size()||total>=65536)return STEAMOHOS_GRAPHICS_PROTOCOL;
        const auto state=Ready(fd,POLLIN,end);if(state)return state;
        iovec io{pending.data()+used,pending.size()-used};
        alignas(cmsghdr) unsigned char ancillary[CMSG_SPACE(16*sizeof(int))]{};
        msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;
        message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);
        const auto n=recvmsg(fd,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<=0)return STEAMOHOS_GRAPHICS_TRANSPORT;
        bool unexpected=message.msg_flags&(MSG_CTRUNC|MSG_TRUNC);
        for(auto* c=CMSG_FIRSTHDR(&message);c;c=NextControl(&message,c)) {
            unexpected=true;
            if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)) {
                const auto count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
                for(size_t i=0;i<count;++i){int received;std::memcpy(&received,CMSG_DATA(c)+i*sizeof(int),sizeof(int));close(received);}
            }
        }
        if(unexpected)return STEAMOHOS_GRAPHICS_PROTOCOL;
        used+=size_t(n);total+=size_t(n);
    }
    return STEAMOHOS_GRAPHICS_TIMEOUT;
}
}
extern "C" int steamohos_probe_graphics_host_v1(const SteamOhosGraphicsHostV1* host,
                                               SteamOhosGraphicsProbeV1* result,uint32_t timeout_ms) {
    if(!result||result->struct_size<sizeof(*result))return STEAMOHOS_GRAPHICS_ABI;
    *result={};result->struct_size=sizeof(*result);result->abi_version=1;result->status=STEAMOHOS_GRAPHICS_ABI;
    if(!host||host->struct_size<sizeof(*host)||host->abi_version!=1||!host->open_wayland||!timeout_ms||timeout_ms>2000)
        return result->status;
    const auto end=Clock::now()+std::chrono::milliseconds(timeout_ms);
    try {
        Fd fd{host->open_wayland(host->context,timeout_ms)};
        result->status=fd.value<0?STEAMOHOS_GRAPHICS_CONNECT:Probe(fd.value,*result,end);
    } catch(...) { result->status=STEAMOHOS_GRAPHICS_TRANSPORT; }
    return result->status;
}
