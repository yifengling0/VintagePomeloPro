#include "audio_bootstrap_relay.h"
#include "protocols/audio_ipc_protocol.h"
#include <cerrno>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace steamohos {
AudioBootstrapRelay::~AudioBootstrapRelay() {
    stop_.store(true);
    if(worker_.joinable())worker_.join();
    if(listener_>=0)close(listener_);
    if(bootstrap_>=0)close(bootstrap_);
    if(!path_.empty())unlink(path_.c_str());
    if(!directory_.empty())rmdir(directory_.c_str());
}
bool AudioBootstrapRelay::Start(int bootstrap,const std::string& root) {
    if(bootstrap<0||listener_>=0)return false;
    const std::string text=root+"/aud-XXXXXX";
    std::vector<char> pattern(text.begin(),text.end());pattern.push_back(0);
    char *directory=mkdtemp(pattern.data());if(!directory)return false;
    directory_=directory;path_=directory_+"/boot";
    bootstrap_=fcntl(bootstrap,F_DUPFD_CLOEXEC,3);
    listener_=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(bootstrap_<0||listener_<0)return false;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(path_.size()>=sizeof(address.sun_path))return false;
    strcpy(address.sun_path,path_.c_str());
    if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))||chmod(path_.c_str(),0600)||listen(listener_,8))return false;
    worker_=std::thread([this]{Run();});return true;
}
void AudioBootstrapRelay::Run() {
    while(!stop_.load()) {
        pollfd ready{listener_,POLLIN,0};int count=poll(&ready,1,100);
        if(count<0&&errno==EINTR)continue;
        if(count<=0)continue;
        int client=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);if(client<0)continue;
        ucred credentials{};socklen_t size=sizeof(credentials);
        if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&credentials,&size)||credentials.uid!=geteuid()){close(client);continue;}
        bool readable=false;
        for(unsigned i=0;i<20&&!stop_.load();++i) {
            pollfd packet{client,POLLIN,0};int n=poll(&packet,1,100);
            if(n>0){readable=(packet.revents&POLLIN)!=0;break;}
            if(n<0&&errno!=EINTR)break;
        }
        if(!readable){close(client);continue;}
        WinehuaAudioBootstrapReq req{};char ancillary[CMSG_SPACE(sizeof(int)*4)]={};
        iovec iov{&req,sizeof(req)};msghdr msg{};msg.msg_iov=&iov;msg.msg_iovlen=1;msg.msg_control=ancillary;msg.msg_controllen=sizeof(ancillary);
        const ssize_t n=recvmsg(client,&msg,MSG_CMSG_CLOEXEC);close(client);
        int passed=-1;unsigned descriptors=0;
        if(n>=0)for(cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c))if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)) {
            auto *fds=reinterpret_cast<int*>(CMSG_DATA(c));size_t total=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<total;++i){++descriptors;if(passed<0)passed=fds[i];else close(fds[i]);}
        }
        if(n==sizeof(req)&&!(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))&&descriptors==1&&
           req.magic==WINEHUA_AUDIO_BOOTSTRAP_MAGIC&&req.version==WINEHUA_AUDIO_PROTOCOL_VERSION&&req.cmd==WINEHUA_AUDIO_BOOTSTRAP_OPEN_CONTROL) {
            // Use kernel credentials for attribution, never a client-supplied PID.
            req.client_pid=credentials.pid;
            char rights[CMSG_SPACE(sizeof(int))]={};msghdr forward{};forward.msg_iov=&iov;forward.msg_iovlen=1;
            forward.msg_control=rights;forward.msg_controllen=sizeof(rights);cmsghdr *c=CMSG_FIRSTHDR(&forward);
            c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&passed,sizeof(passed));
            sendmsg(bootstrap_,&forward,MSG_NOSIGNAL|MSG_DONTWAIT);
        }
        if(passed>=0)close(passed);
    }
}
}
