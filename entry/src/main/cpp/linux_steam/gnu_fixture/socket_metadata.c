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
#include <time.h>
#include <arpa/inet.h>
#include <dirent.h>

struct Request {uint32_t version;int32_t descriptor;uint64_t inode;};
struct EndpointPair {struct sockaddr_storage local,peer;};
static int tcp_pair(int fd,struct EndpointPair *pair) {
    int type=0;socklen_t size=sizeof(type);
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_STREAM)return -1;
    size=sizeof(pair->local);
    if(getsockname(fd,(struct sockaddr*)&pair->local,&size))return -1;
    size=sizeof(pair->peer);
    return getpeername(fd,(struct sockaddr*)&pair->peer,&size);
}
static int loopback_pair(const struct EndpointPair *pair) {
    if(pair->local.ss_family!=pair->peer.ss_family)return 0;
    if(pair->local.ss_family==AF_INET) {
        const struct sockaddr_in *a=(const void*)&pair->local,*b=(const void*)&pair->peer;
        return (ntohl(a->sin_addr.s_addr)>>24)==127&&(ntohl(b->sin_addr.s_addr)>>24)==127&&a->sin_port&&b->sin_port;
    }
    if(pair->local.ss_family==AF_INET6) {
        const struct sockaddr_in6 *a=(const void*)&pair->local,*b=(const void*)&pair->peer;
        return IN6_IS_ADDR_LOOPBACK(&a->sin6_addr)&&IN6_IS_ADDR_LOOPBACK(&b->sin6_addr)&&a->sin6_port&&b->sin6_port;
    }
    return 0;
}
static int same_address(const struct sockaddr_storage *a,const struct sockaddr_storage *b) {
    if(a->ss_family!=b->ss_family)return 0;
    if(a->ss_family==AF_INET) {
        const struct sockaddr_in *x=(const void*)a,*y=(const void*)b;
        return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;
    }
    if(a->ss_family==AF_INET6) {
        const struct sockaddr_in6 *x=(const void*)a,*y=(const void*)b;
        return x->sin6_port==y->sin6_port&&x->sin6_scope_id==y->sin6_scope_id&&
            !memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(x->sin6_addr));
    }
    return 0;
}
static int opposite_pair(const struct EndpointPair *a,const struct EndpointPair *b) {
    return loopback_pair(a)&&loopback_pair(b)&&same_address(&a->local,&b->peer)&&same_address(&a->peer,&b->local);
}
// Enumerate only this process. OHOS may deny another same-UID process' /proc
// FD links. Return a duplicate of the live kernel endpoint, not metadata text.
static int find_peer(const struct EndpointPair *pair) {
    if(!loopback_pair(pair)){errno=EINVAL;return -1;}
    DIR *directory=opendir("/proc/self/fd");if(!directory)return -1;
    struct dirent *entry;unsigned examined=0;int found=-1;
    while(examined++<4096&&(entry=readdir(directory))) {
        if(!*entry->d_name||strspn(entry->d_name,"0123456789")!=strlen(entry->d_name))continue;
        long number=strtol(entry->d_name,NULL,10);
        if(number<0||number>1048576||number==dirfd(directory))continue;
        int copy=fcntl((int)number,F_DUPFD_CLOEXEC,0);if(copy<0)continue;
        struct EndpointPair remote={0};
        if(!tcp_pair(copy,&remote)&&opposite_pair(pair,&remote)){found=copy;break;}
        close(copy);
    }
    closedir(directory);if(found<0)errno=ENOENT;return found;
}
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
// Stream operations may be interrupted or fragmented. Use one absolute
// deadline for the entire authenticated query, including its ancillary FD.
static int64_t milliseconds(void) {
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return -1;
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
static int wait_io(int fd,short events,int64_t deadline) {
    for(;;) {
        int64_t now=milliseconds();if(now<0)return -1;
        if(now>=deadline){errno=ETIMEDOUT;return -1;}
        struct pollfd ready={.fd=fd,.events=events};
        int result=poll(&ready,1,(int)(deadline-now));
        if(result>0)return 0;
        if(result<0&&errno==EINTR)continue;
        if(!result)errno=ETIMEDOUT;
        return -1;
    }
}
static int transfer(int fd,void *buffer,size_t count,int writing,int64_t deadline) {
    char *at=buffer;
    while(count) {
        if(wait_io(fd,writing?POLLOUT:POLLIN,deadline))return -1;
        ssize_t n=writing?send(fd,at,count,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,at,count,MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0){if(!n)errno=EIO;return -1;}
        at+=n;count-=(size_t)n;
    }
    return 0;
}
static int receive_reply(int channel,int *response,int *descriptor,int64_t deadline) {
    size_t used=0;unsigned received=0;
    while(used<sizeof(*response)) {
        if(wait_io(channel,POLLIN,deadline))return -1;
        struct iovec io={.iov_base=(char*)response+used,.iov_len=sizeof(*response)-used};
        union {struct cmsghdr aligned;char bytes[CMSG_SPACE(4*sizeof(int))];} ancillary={0};
        struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=sizeof(ancillary.bytes)};
        ssize_t n=recvmsg(channel,&message,MSG_CMSG_CLOEXEC|MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<0)return -1;
        for(struct cmsghdr *h=CMSG_FIRSTHDR(&message);h;h=CMSG_NXTHDR(&message,h)) {
            if(h->cmsg_level!=SOL_SOCKET||h->cmsg_type!=SCM_RIGHTS||h->cmsg_len<CMSG_LEN(0))continue;
            size_t count=(h->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<count;++i) {
                int copy;memcpy(&copy,(char*)CMSG_DATA(h)+i*sizeof(int),sizeof(copy));
                if(!received)*descriptor=copy;else close(copy);
                ++received;
            }
        }
        if(!n||message.msg_flags&(MSG_CTRUNC|MSG_TRUNC)||received>1){errno=EPROTO;return -1;}
        used+=(size_t)n;
    }
    return (int)received;
}
static void serve(int channel) {
    struct Request request;struct stat status;int error=0;
    if(peer_is(channel,0))return;
    int64_t now=milliseconds();if(now<0)return;
    const int64_t deadline=now+1000;
    if(transfer(channel,&request,sizeof(request),0,deadline))return;
    int duplicate=-1;
    if(request.version==1&&request.descriptor>=0&&request.inode) {
        duplicate=fcntl(request.descriptor,F_DUPFD_CLOEXEC,0);
        if(duplicate<0)error=errno;
        else if(socket_stat(duplicate,request.inode,&status))error=errno;
    } else if(request.version==2&&request.descriptor==-1&&!request.inode) {
        struct EndpointPair pair;
        if(transfer(channel,&pair,sizeof(pair),0,deadline))return;
        duplicate=find_peer(&pair);if(duplicate<0)error=errno;
    } else error=EINVAL;
    struct iovec io={.iov_base=&error,.iov_len=sizeof(error)};
    union {struct cmsghdr aligned;char bytes[CMSG_SPACE(sizeof(int))];} ancillary={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1};
    if(!error) {
        message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
        struct cmsghdr *header=CMSG_FIRSTHDR(&message);
        header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header),&duplicate,sizeof(duplicate));
    }
    for(;;) {
        if(wait_io(channel,POLLOUT,deadline))break;
        ssize_t n=sendmsg(channel,&message,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        // SCM_RIGHTS is sent only with the first successful byte. Continue
        // a short stream write without sending another duplicate descriptor.
        if(n>0&&(size_t)n<sizeof(error))
            transfer(channel,(char*)&error+n,sizeof(error)-(size_t)n,1,deadline);
        break;
    }
    if(duplicate>=0)close(duplicate);
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
static int query_socket(pid_t pid,const struct Request *request,const struct EndpointPair *pair) {
    struct sockaddr_un path={.sun_family=AF_UNIX};
    if(endpoint_path(pid,path.sun_path,sizeof(path.sun_path),0))return -1;
    int channel=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(channel<0)return -1;
    int fd=-1,error=0;
    int64_t now=milliseconds();if(now<0){error=errno;goto done;}
    const int64_t deadline=now+1000;
    if(connect(channel,(struct sockaddr*)&path,sizeof(path))) {
        if(errno!=EINPROGRESS){error=errno;goto done;}
        if(wait_io(channel,POLLOUT,deadline)){error=errno;goto done;}
        socklen_t size=sizeof(error);
        if(getsockopt(channel,SOL_SOCKET,SO_ERROR,&error,&size)){error=errno;goto done;}
        if(error)goto done;
    }
    if(peer_is(channel,pid)){error=errno;goto done;}
    if(transfer(channel,(void*)request,sizeof(*request),1,deadline)||
       (pair&&transfer(channel,(void*)pair,sizeof(*pair),1,deadline))){error=errno;goto done;}
    int response=EIO;
    int received=receive_reply(channel,&response,&fd,deadline);
    if(received!=1||response){error=received<0?errno:response?response:EPROTO;goto done;}
    if(request->version==1) {
        struct stat status;if(socket_stat(fd,request->inode,&status)){error=errno;goto done;}
    } else {
        struct EndpointPair remote={0};
        if(tcp_pair(fd,&remote)||!opposite_pair(pair,&remote)){error=EPROTO;goto done;}
    }
done:
    close(channel);
    if(error){if(fd>=0)close(fd);errno=error;return -1;}
    return fd;
}
int SteamOhosBorrowSocket(pid_t pid,int descriptor,uint64_t inode) {
    if(!enabled()||pid<=0||descriptor<0||!inode){errno=ENOTSUP;return -1;}
    if(pid==getpid()) {
        int fd=fcntl(descriptor,F_DUPFD_CLOEXEC,0);struct stat status;
        if(fd>=0&&socket_stat(fd,inode,&status)){int error=errno;close(fd);errno=error;return -1;}
        return fd;
    }
    const struct Request request={.version=1,.descriptor=descriptor,.inode=inode};
    return query_socket(pid,&request,NULL);
}
int SteamOhosFindPeerSocket(pid_t pid,int descriptor) {
    if(!enabled()||pid<=0||descriptor<0){errno=ENOTSUP;return -1;}
    struct EndpointPair pair={0};
    if(tcp_pair(descriptor,&pair)||!loopback_pair(&pair)){errno=EINVAL;return -1;}
    const struct Request request={.version=2,.descriptor=-1,.inode=0};
    return query_socket(pid,&request,&pair);
}
__attribute__((destructor)) static void finalize(void) {
    if(owner!=getpid()||listener<0)return;
    __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);pthread_join(worker,NULL);
    close(listener);listener=-1;unlink(endpoint);
}
