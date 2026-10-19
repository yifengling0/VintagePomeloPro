// Live descriptor queries between explicitly opted-in GNU app processes.
// Metadata always comes from a kernel-duplicated FD, never a cached record.
#define _GNU_SOURCE 1
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct Request {uint32_t version;int32_t descriptor;uint64_t inode;};
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int listener=-1,stopping;
static pid_t owner;
static char endpoint[sizeof(((struct sockaddr_un*)0)->sun_path)];

static int enabled(void) {
    const char *value=getenv("STEAMOHOS_SOCKET_METADATA");
    return value&&!strcmp(value,"1");
}
static int endpoint_path(pid_t pid,char *path,size_t capacity,int create) {
    const char *root=getenv("STEAMOHOS_SOCKET_METADATA_ROOT");char directory[PATH_MAX];
    if(!root)root=getenv("STEAMOHOS_IPC_ROOT");
    if(!enabled()||!root||root[0]!='/'||pid<=0){errno=ENOTSUP;return -1;}
    int n=snprintf(directory,sizeof(directory),"%s/socket-metadata",root);
    if(n<0||n>=(int)sizeof(directory)){errno=ENAMETOOLONG;return -1;}
    if(create&&mkdir(directory,0700)&&errno!=EEXIST)return -1;
    struct stat status;
    if(lstat(directory,&status))return -1;
    if(!S_ISDIR(status.st_mode)||status.st_uid!=geteuid()||(status.st_mode&0077)){
        errno=EACCES;return -1;
    }
    n=snprintf(path,capacity,"%s/%d",directory,pid);
    if(n<0||(size_t)n>=capacity){errno=ENAMETOOLONG;return -1;}
    return 0;
}
static int peer_is(int fd,pid_t pid) {
    struct ucred credential;socklen_t length=sizeof(credential);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credential,&length))return -1;
    if(length!=sizeof(credential)||credential.uid!=geteuid()||credential.pid<=0||
       (pid>0&&credential.pid!=pid)){errno=EACCES;return -1;}
    return 0;
}
static int socket_stat(int fd,uint64_t inode,struct stat *status) {
    if(syscall(SYS_fstat,fd,status))return -1;
    if(!S_ISSOCK(status->st_mode)||(uint64_t)status->st_ino!=inode){errno=ESTALE;return -1;}
    struct sockaddr_storage address;socklen_t size=sizeof(address);
    if(getsockname(fd,(struct sockaddr*)&address,&size))return -1;
    if(address.ss_family!=AF_INET&&address.ss_family!=AF_INET6){errno=EAFNOSUPPORT;return -1;}
    return 0;
}
static void serve(int channel) {
    struct Request request;struct stat status;int error=0;
    struct timeval timeout={.tv_sec=0,.tv_usec=150000};
    setsockopt(channel,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(channel,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(peer_is(channel,0))return;
    ssize_t n=recv(channel,&request,sizeof(request),MSG_WAITALL);
    if(n!=sizeof(request))return;
    if(request.version!=1||request.descriptor<0||request.inode==0)error=EINVAL;
    else if(socket_stat(request.descriptor,request.inode,&status))error=errno;
    struct iovec io={.iov_base=&error,.iov_len=sizeof(error)};
    union {struct cmsghdr aligned;char bytes[CMSG_SPACE(sizeof(int))];} ancillary={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1};
    if(!error) {
        message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
        struct cmsghdr *header=CMSG_FIRSTHDR(&message);
        header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header),&request.descriptor,sizeof(request.descriptor));
    }
    sendmsg(channel,&message,MSG_NOSIGNAL);
}
static void *run(void *unused) {
    (void)unused;
    while(!__atomic_load_n(&stopping,__ATOMIC_ACQUIRE)) {
        struct pollfd ready={.fd=listener,.events=POLLIN};
        if(poll(&ready,1,50)<=0)continue;
        int channel=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
        if(channel<0)continue;
        serve(channel);close(channel);
    }
    return NULL;
}
static void before_fork(void){pthread_mutex_lock(&guard);}
static void after_parent(void){pthread_mutex_unlock(&guard);}
static void after_child(void) {
    if(listener>=0)close(listener);
    listener=-1;owner=0;endpoint[0]=0;stopping=0;pthread_mutex_unlock(&guard);
}
__attribute__((constructor)) static void initialize(void) {
    pthread_atfork(before_fork,after_parent,after_child);
}
void SteamOhosObserveSocket(int fd) {
    int saved=errno;
    if(!enabled()){errno=saved;return;}
    struct sockaddr_storage address;socklen_t size=sizeof(address);
    if(getsockname(fd,(struct sockaddr*)&address,&size)||
       (address.ss_family!=AF_INET&&address.ss_family!=AF_INET6)){errno=saved;return;}
    pthread_mutex_lock(&guard);
    if(owner!=getpid()&&endpoint_path(getpid(),endpoint,sizeof(endpoint),1)==0) {
        struct stat status;
        if(lstat(endpoint,&status)==0) {
            // Only this process' stale reserved endpoint may be replaced.
            if(status.st_uid!=geteuid()||!S_ISSOCK(status.st_mode))goto done;
            unlink(endpoint);
        } else if(errno!=ENOENT)goto done;
        listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        struct sockaddr_un path={.sun_family=AF_UNIX};strcpy(path.sun_path,endpoint);
        if(listener<0||bind(listener,(struct sockaddr*)&path,sizeof(path))||listen(listener,8))goto failed;
        owner=getpid();stopping=0;
        int result=pthread_create(&worker,NULL,run,NULL);
        if(result){owner=0;errno=result;goto failed;}
        goto done;
failed:
        if(listener>=0)close(listener);
        listener=-1;unlink(endpoint);
    }
done:
    pthread_mutex_unlock(&guard);errno=saved;
}

// The caller closes the returned duplicate. Both Unix peer credentials and
// the kernel socket inode must match; stale descriptors are rejected.
int SteamOhosBorrowSocket(pid_t pid,int descriptor,uint64_t inode) {
    if(!enabled()||pid<=0||descriptor<0||!inode){errno=ENOTSUP;return -1;}
    if(pid==getpid()) {
        int fd=fcntl(descriptor,F_DUPFD_CLOEXEC,0);struct stat status;
        if(fd>=0&&socket_stat(fd,inode,&status)){int error=errno;close(fd);errno=error;return -1;}
        return fd;
    }
    struct sockaddr_un path={.sun_family=AF_UNIX};
    if(endpoint_path(pid,path.sun_path,sizeof(path.sun_path),0))return -1;
    int channel=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(channel<0)return -1;
    int fd=-1,error=0;
    if(connect(channel,(struct sockaddr*)&path,sizeof(path))) {
        if(errno!=EINPROGRESS){error=errno;goto done;}
        struct pollfd ready={.fd=channel,.events=POLLOUT};
        if(poll(&ready,1,150)<=0){error=ETIMEDOUT;goto done;}
        socklen_t size=sizeof(error);
        if(getsockopt(channel,SOL_SOCKET,SO_ERROR,&error,&size)){error=errno;goto done;}
        if(error)goto done;
    }
    if(peer_is(channel,pid)){error=errno;goto done;}
    int flags=fcntl(channel,F_GETFL);if(flags<0||fcntl(channel,F_SETFL,flags&~O_NONBLOCK)){error=errno;goto done;}
    struct timeval timeout={.tv_sec=0,.tv_usec=150000};
    setsockopt(channel,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(channel,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct Request request={.version=1,.descriptor=descriptor,.inode=inode};
    if(send(channel,&request,sizeof(request),MSG_NOSIGNAL)!=sizeof(request)){error=errno?errno:EIO;goto done;}
    int response=EIO;struct iovec io={.iov_base=&response,.iov_len=sizeof(response)};
    union {struct cmsghdr aligned;char bytes[CMSG_SPACE(4*sizeof(int))];} ancillary={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=sizeof(ancillary.bytes)};
    ssize_t n=recvmsg(channel,&message,MSG_CMSG_CLOEXEC|MSG_WAITALL);
    int received=0;
    for(struct cmsghdr *h=CMSG_FIRSTHDR(&message);h;h=CMSG_NXTHDR(&message,h)) {
        if(h->cmsg_level!=SOL_SOCKET||h->cmsg_type!=SCM_RIGHTS||h->cmsg_len<CMSG_LEN(0))continue;
        size_t count=(h->cmsg_len-CMSG_LEN(0))/sizeof(int);
        for(size_t i=0;i<count;++i) {
            int copy;memcpy(&copy,(char*)CMSG_DATA(h)+i*sizeof(int),sizeof(copy));
            if(!received)fd=copy;else close(copy);
            ++received;
        }
    }
    if(n!=sizeof(response)||message.msg_flags&(MSG_CTRUNC|MSG_TRUNC)||received!=1||response) {
        error=n<0?errno:response?response:EPROTO;goto done;
    }
    struct stat status;if(socket_stat(fd,inode,&status)){error=errno;goto done;}
done:
    close(channel);
    if(error){if(fd>=0)close(fd);errno=error;return -1;}
    return fd;
}
__attribute__((destructor)) static void finalize(void) {
    if(owner!=getpid()||listener<0)return;
    __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);pthread_join(worker,NULL);
    close(listener);listener=-1;unlink(endpoint);
}
