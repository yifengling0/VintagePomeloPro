#include "host.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace {
void Word(std::vector<unsigned char>& b,uint32_t v) {
    const auto size=b.size();b.resize(size+4);std::memcpy(b.data()+size,&v,4);
}
void Global(std::vector<unsigned char>& b,uint32_t id,const char* name,uint32_t version) {
    const uint32_t bytes=std::strlen(name)+1,padded=(bytes+3)&~3u;
    Word(b,2);Word(b,((20+padded)<<16));Word(b,id);Word(b,bytes);
    const auto at=b.size();b.resize(at+padded);std::memcpy(b.data()+at,name,bytes);Word(b,version);
}
void Done(std::vector<unsigned char>& b) { Word(b,3);Word(b,12u<<16);Word(b,42); }
struct Context { int fd;unsigned calls=0; };
int Open(void* p,uint32_t) { auto& c=*static_cast<Context*>(p);++c.calls;const auto fd=c.fd;c.fd=-1;return fd; }
bool Trial(unsigned scenario) {
    int fds[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds))return false;
    Context c{fds[0]};bool request=false;
    std::thread server([&] {
        unsigned char input[24]{};size_t used=0;
        while(used<sizeof(input)) {
            pollfd p{fds[1],POLLIN,0};if(poll(&p,1,1000)<=0)break;
            const auto n=recv(fds[1],input+used,sizeof(input)-used,0);if(n<=0)break;used+=size_t(n);
        }
        const uint32_t wanted[]={1,(12u<<16)|1,2,1,12u<<16,3};request=used==24&&!std::memcmp(input,wanted,24);
        std::vector<unsigned char> message;
        Global(message,10,"wl_compositor",4);Global(message,11,"wl_shm",1);
        Global(message,12,"xdg_wm_base",3);
        if(scenario!=2)Global(message,13,"wl_seat",5);
        Global(message,14,"wl_output",3);Global(message,15,"wp_viewporter",1);
        if(scenario==3){Word(message,2);Word(message,(12u<<16)|1);Word(message,12);}
        if(scenario==4)Global(message,10,"duplicate_name",1);
        if(scenario==5){message[4]=1;message[5]=0;}
        if(scenario==6){message[6]=0xff;message[7]=0xff;}
        if(scenario==7){message[16+sizeof("wl_compositor")-1]='x';}
        if(scenario!=8)Done(message);
        if(scenario==9) {
            int descriptor=open("/dev/null",O_RDONLY|O_CLOEXEC);
            iovec io{message.data(),message.size()};alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))]{};
            msghdr msg{};msg.msg_iov=&io;msg.msg_iovlen=1;msg.msg_control=control;msg.msg_controllen=sizeof(control);
            auto* h=CMSG_FIRSTHDR(&msg);h->cmsg_level=SOL_SOCKET;h->cmsg_type=SCM_RIGHTS;h->cmsg_len=CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(h),&descriptor,sizeof(descriptor));sendmsg(fds[1],&msg,MSG_NOSIGNAL);close(descriptor);
        } else {
            size_t at=0;
            while(at<message.size()) {
                const auto n=send(fds[1],message.data()+at,scenario==1?1:message.size()-at,MSG_NOSIGNAL);
                if(n<=0)break;
                at+=size_t(n);
            }
        }
        if(scenario==8){pollfd p{fds[1],POLLIN,0};poll(&p,1,1000);}
        close(fds[1]);
    });
    SteamOhosGraphicsHostV1 host{sizeof(host),1,&c,Open};
    SteamOhosGraphicsProbeV1 result{};result.struct_size=sizeof(result);
    const auto status=steamohos_probe_graphics_host_v1(&host,&result,scenario==8?100:1000);
    server.join();
    const int expected=scenario<=1?STEAMOHOS_GRAPHICS_VERIFIED:(scenario==2||scenario==3)?STEAMOHOS_GRAPHICS_MISSING_GLOBALS
        :scenario==8?STEAMOHOS_GRAPHICS_TIMEOUT:STEAMOHOS_GRAPHICS_PROTOCOL;
    const bool closed=fcntl(fds[0],F_GETFD)==-1&&errno==EBADF;
    const bool ok=request&&status==expected&&result.status==unsigned(expected)&&closed&&c.calls==1
        &&!result.window_configured&&!result.frame_presented&&!result.input_verified&&!result.steam_client_started;
    if(!ok)std::cerr<<"scenario="<<scenario<<" status="<<status<<" expected="<<expected<<" request="<<request<<'\n';
    return ok;
}
}
int main() {
    auto descriptors=[] { size_t n=0;auto* dir=opendir("/proc/self/fd");if(!dir)return size_t(0);
        while(readdir(dir))++n;
        closedir(dir);return n;
    };
    const auto before=descriptors();
    bool ok=true;
    for(unsigned scenario=0;scenario<10;++scenario)ok=Trial(scenario)&&ok;
    SteamOhosGraphicsProbeV1 result{};result.struct_size=sizeof(result);
    Context c{-1};SteamOhosGraphicsHostV1 host{sizeof(host),2,&c,Open};
    ok&=steamohos_probe_graphics_host_v1(&host,&result,100)==STEAMOHOS_GRAPHICS_ABI&&c.calls==0;
    host.abi_version=1;
    ok&=steamohos_probe_graphics_host_v1(&host,&result,2001)==STEAMOHOS_GRAPHICS_ABI&&c.calls==0;
    ok&=steamohos_probe_graphics_host_v1(&host,&result,100)==STEAMOHOS_GRAPHICS_CONNECT&&c.calls==1;
    const auto after=descriptors();ok&=before&&before==after;
    std::cout<<"{\"graphics_host_contract_verified\":"<<(ok?"true":"false")<<",\"wire_scenarios\":10,\"abi_controls\":3,\"fd_leak\":"<<(before==after?"false":"true")<<"}\n";
    return ok?0:1;
}
