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
#include <signal.h>
#include <netinet/tcp.h>
#include <sys/prctl.h>
#include "socket_query_protocol.h"

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
static pthread_mutex_t lifecycle=PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static int listener=-1,stopping;
static int wake[2]={-1,-1};
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
static int socket_stat(int fd,uint64_t inode,struct stat *status);
static int tcp_socket(int fd) {
    int type=0;socklen_t size=sizeof(type);
    struct tcp_info info;
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)||type!=SOCK_STREAM){errno=EAFNOSUPPORT;return -1;}
    size=sizeof(info);
    return getsockopt(fd,IPPROTO_TCP,TCP_INFO,&info,&size);
}
static int number_order(const void *a,const void *b) {
    int x=*(const int*)a,y=*(const int*)b;return (x>y)-(x<y);
}
static int next_tcp(int minimum,struct SteamOhosSocketRecord *record) {
    // As in VPP's peer lookup, the target enumerates only its OWN FDs. Pin
    // each candidate before inspecting it; concurrent close/reuse cannot
    // change the endpoint sent with SCM_RIGHTS. No foreign proc is needed.
    DIR *directory=opendir("/proc/self/fd");if(!directory)return -1;
    int numbers[8192];unsigned count=0;int error=0;struct dirent *entry;
    errno=0;
    while((entry=readdir(directory))) {
        if(!*entry->d_name||strspn(entry->d_name,"0123456789")!=strlen(entry->d_name))continue;
        char *end;long value=strtol(entry->d_name,&end,10);
        if(*end||value<minimum||value>=INT_MAX||value==dirfd(directory))continue;
        if(count==sizeof(numbers)/sizeof(numbers[0])){error=EOVERFLOW;break;}
        numbers[count++]=(int)value;
    }
    if(!error&&errno)error=errno;
    closedir(directory);
    if(error){errno=error;return -1;}
    qsort(numbers,count,sizeof(numbers[0]),number_order);
    for(unsigned i=0;i<count;++i) {
        int fd=fcntl(numbers[i],F_DUPFD_CLOEXEC,0);if(fd<0)continue;
        struct stat status;
        if(!syscall(SYS_fstat,fd,&status)&&S_ISSOCK(status.st_mode)&&
           !socket_stat(fd,status.st_ino,&status)&&!tcp_socket(fd)) {
            record->descriptor=numbers[i];record->inode=status.st_ino;
            record->pid=getpid();record->ppid=getppid();record->pgid=getpgrp();
            record->uid=geteuid();
            // This runs on the broker thread. prctl alone would name that
            // thread, whereas upstream lsof selects the process leader.
            int comm=open("/proc/self/comm",O_RDONLY|O_CLOEXEC);
            ssize_t length=comm>=0?read(comm,record->command,sizeof(record->command)-1):-1;
            if(comm>=0)close(comm);
            if(length>0) {
                if(record->command[length-1]=='\n')--length;
                record->command[length]=0;
            } else prctl(PR_GET_NAME,record->command,0,0,0);
            record->command[sizeof(record->command)-1]=0;
            return fd;
        }
        close(fd);
    }
    errno=ENOENT;return -1;
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
static int receive_record(int channel,void *response,size_t response_size,int *descriptor,int64_t deadline) {
    size_t used=0;unsigned received=0;
    while(used<response_size) {
        if(wait_io(channel,POLLIN,deadline))return -1;
        struct iovec io={.iov_base=(char*)response+used,.iov_len=response_size-used};
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

// One incomplete peer must not consume every other peer's transaction budget.
// Keep a fixed set of nonblocking channels; neither threads nor FDs grow with
// incoming queries. The caller and each accepted channel retain a 1s deadline.
#define QUERY_CHANNELS 32
struct QueryChannel {
    int fd, duplicate, response, replying, rights_sent;
    uint32_t reserved;
    uint64_t directory_inode;
    size_t used;
    int64_t deadline;
    struct Request request;
    struct EndpointPair pair;
    struct SteamOhosSocketRecord record;
};
static struct QueryChannel channels[QUERY_CHANNELS];
static int own_stat_snapshot(uint64_t *inode) {
    int directory=open("/proc/self",O_RDONLY|O_DIRECTORY|O_CLOEXEC);struct stat status;
    if(directory<0)return -1;
    int result=fstat(directory,&status),error=errno;close(directory);
    if(result){errno=error;return -1;}*inode=status.st_ino;
    int source=open("/proc/self/stat",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(source<0)return -1;
    char data[4096];ssize_t bytes=pread(source,data,sizeof(data),0);error=errno;close(source);
    if(bytes<=0||bytes==(ssize_t)sizeof(data)){errno=bytes<0?error:EOVERFLOW;return -1;}
    char path[sizeof(endpoint)+48];static unsigned serial;
    int n=snprintf(path,sizeof(path),"%s.stat-%u",endpoint,__atomic_fetch_add(&serial,1,__ATOMIC_RELAXED));
    if(n<0||n>=(int)sizeof(path)){errno=ENAMETOOLONG;return -1;}
    int output=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600),input=-1;
    if(output<0)return -1;
    if(write(output,data,(size_t)bytes)!=bytes)error=errno?errno:EIO;
    else if((input=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW))<0)error=errno;
    else {
        struct stat before,after;
        if(fstat(output,&before)||fstat(input,&after))error=errno;
        else error=before.st_dev!=after.st_dev||before.st_ino!=after.st_ino?ESTALE:0;
    }
    if(unlink(path)&&!error)error=errno;
    close(output);
    if(error){if(input>=0)close(input);errno=error;return -1;}return input;
}
static void close_query(struct QueryChannel *q) {
    if(q->fd>=0)close(q->fd);
    if(q->duplicate>=0)close(q->duplicate);
    memset(q,0,sizeof(*q));q->fd=q->duplicate=-1;
}
static void resolve_query(struct QueryChannel *q) {
    struct stat status;
    q->duplicate=-1;q->response=0;
    if(q->request.version==1&&q->request.descriptor>=0&&q->request.inode) {
        q->duplicate=fcntl(q->request.descriptor,F_DUPFD_CLOEXEC,0);
        if(q->duplicate<0)q->response=errno;
        else if(socket_stat(q->duplicate,q->request.inode,&status))q->response=errno;
    } else if(q->request.version==2&&q->request.descriptor==-1&&!q->request.inode) {
        q->duplicate=find_peer(&q->pair);if(q->duplicate<0)q->response=errno;
    } else if(q->request.version==3&&q->request.descriptor>=0&&!q->request.inode) {
        q->duplicate=next_tcp(q->request.descriptor,&q->record);
        if(q->duplicate<0)q->response=errno;
        q->record.error=q->response;
    } else if(q->request.version==5&&q->request.descriptor==0&&!q->request.inode) {
        q->duplicate=own_stat_snapshot(&q->directory_inode);if(q->duplicate<0)q->response=errno;
    } else q->response=EINVAL;
    q->replying=1;q->used=0;
}
static void step_query(struct QueryChannel *q,short events,int64_t current) {
    if(current>=q->deadline||(events&(POLLERR|POLLNVAL))){close_query(q);return;}
    if(!q->replying&&(events&(POLLIN|POLLHUP))) {
        // At most two reads: fixed request header, then optional endpoint pair.
        for(unsigned step=0;step<2&&!q->replying;++step) {
            size_t size=sizeof(q->request);
            void *buffer=&q->request;
            if(q->used>=size&&q->request.version==2) {
                buffer=&q->pair;size+=sizeof(q->pair);
            }
            size_t offset=q->used-(buffer==(void*)&q->pair?sizeof(q->request):0);
            ssize_t n=recv(q->fd,(char*)buffer+offset,size-q->used,MSG_DONTWAIT);
            if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))return;
            if(n<=0){close_query(q);return;}
            q->used+=(size_t)n;
            if(q->used<sizeof(q->request))return;
            if(q->request.version==2&&q->used<sizeof(q->request)+sizeof(q->pair))continue;
            resolve_query(q);
        }
    }
    if(!q->replying)return;
    struct {int32_t error;uint32_t reserved;uint64_t directory_inode;} response={q->response,0,q->directory_inode};
    const size_t reply_size=q->request.version==3?sizeof(q->record):q->request.version==5?sizeof(response):sizeof(response.error);
    void *record=q->request.version==3?(void*)&q->record:(void*)&response;
    struct iovec io={.iov_base=(char*)record+q->used,.iov_len=reply_size-q->used};
    union {struct cmsghdr aligned;char bytes[CMSG_SPACE(sizeof(int))];} ancillary={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1};
    if(!q->response&&!q->rights_sent) {
        message.msg_control=ancillary.bytes;message.msg_controllen=sizeof(ancillary.bytes);
        struct cmsghdr *header=CMSG_FIRSTHDR(&message);
        header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header),&q->duplicate,sizeof(q->duplicate));
    }
    ssize_t n=sendmsg(q->fd,&message,MSG_NOSIGNAL|MSG_DONTWAIT);
    if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))return;
    if(n<=0){close_query(q);return;}
    q->rights_sent=1;q->used+=(size_t)n;
    if(q->used==reply_size)close_query(q);
}
static void *run(void *unused) {
    (void)unused;
    while(!__atomic_load_n(&stopping,__ATOMIC_ACQUIRE)) {
        struct pollfd ready[QUERY_CHANNELS+2];
        int available=0;
        for(unsigned i=0;i<QUERY_CHANNELS;++i) {
            available+=channels[i].fd<0;
            ready[i+1]=(struct pollfd){.fd=channels[i].fd,.events=channels[i].replying?POLLOUT:POLLIN};
        }
        ready[0]=(struct pollfd){.fd=available?listener:-1,.events=POLLIN};
        ready[QUERY_CHANNELS+1]=(struct pollfd){.fd=wake[0],.events=POLLIN};
        if(poll(ready,QUERY_CHANNELS+2,1000)<0&&errno!=EINTR)break;
        if(ready[QUERY_CHANNELS+1].revents||__atomic_load_n(&stopping,__ATOMIC_ACQUIRE))break;
        // Serialize state updates with fork, never while waiting on peer I/O.
        pthread_mutex_lock(&guard);
        int64_t current=milliseconds();
        for(unsigned i=0;i<QUERY_CHANNELS;++i)
            if(channels[i].fd>=0)step_query(&channels[i],ready[i+1].revents,current);
        if(available&&(ready[0].revents&POLLIN)) {
            for(unsigned i=0;i<QUERY_CHANNELS;++i)if(channels[i].fd<0) {
                int channel=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
                if(channel<0)break;
                if(peer_is(channel,0)){close(channel);continue;}
                channels[i].fd=channel;channels[i].deadline=milliseconds()+1000;
            }
        }
        pthread_mutex_unlock(&guard);
    }
    pthread_mutex_lock(&guard);
    for(unsigned i=0;i<QUERY_CHANNELS;++i)close_query(&channels[i]);
    pthread_mutex_unlock(&guard);
    return NULL;
}
static void before_fork(void){pthread_mutex_lock(&lifecycle);pthread_mutex_lock(&guard);}
static void after_parent(void){pthread_mutex_unlock(&guard);pthread_mutex_unlock(&lifecycle);}
static void after_child(void) {
    if(listener>=0)close(listener);
    if(wake[0]>=0)close(wake[0]);
    if(wake[1]>=0)close(wake[1]);
    wake[0]=wake[1]=-1;
    for(unsigned i=0;i<QUERY_CHANNELS;++i)close_query(&channels[i]);
    listener=-1;owner=0;endpoint[0]=0;stopping=0;pthread_mutex_unlock(&guard);pthread_mutex_unlock(&lifecycle);
}
__attribute__((constructor)) static void initialize(void) {
    for(unsigned i=0;i<QUERY_CHANNELS;++i)channels[i].fd=channels[i].duplicate=-1;
    pthread_atfork(before_fork,after_parent,after_child);
}
void SteamOhosObserveSocket(int fd) {
    int saved=errno;
    if(!enabled()){errno=saved;return;}
    struct sockaddr_storage address;socklen_t size=sizeof(address);
    if(getsockname(fd,(struct sockaddr*)&address,&size)){errno=saved;return;}
    if(address.ss_family!=AF_INET&&address.ss_family!=AF_INET6) {
        const char *process=getenv("STEAMOHOS_PROCESS_METADATA");
        struct sockaddr_un peer={0};size=sizeof(peer);
        if(address.ss_family!=AF_UNIX||!process||strcmp(process,"1")||
           getpeername(fd,(struct sockaddr*)&peer,&size)||size<=sizeof(sa_family_t)+1){errno=saved;return;}
    }
    pthread_mutex_lock(&guard);
    if(__atomic_load_n(&stopping,__ATOMIC_ACQUIRE))goto done;
    if(owner!=getpid()&&endpoint_path(getpid(),endpoint,sizeof(endpoint),1)==0) {
        struct stat status;
        if(lstat(endpoint,&status)==0) {
            // Only this process' stale reserved endpoint may be replaced.
            if(status.st_uid!=geteuid()||!S_ISSOCK(status.st_mode))goto done;
            unlink(endpoint);
        } else if(errno!=ENOENT)goto done;
        listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        struct sockaddr_un path={.sun_family=AF_UNIX};strcpy(path.sun_path,endpoint);
        if(listener<0||bind(listener,(struct sockaddr*)&path,sizeof(path))||listen(listener,QUERY_CHANNELS))goto failed;
        if(pipe2(wake,O_CLOEXEC|O_NONBLOCK))goto failed;
        owner=getpid();stopping=0;
        pthread_attr_t attributes;int result=pthread_attr_init(&attributes);
        if(result){owner=0;errno=result;goto failed;}
        sigset_t blocked;
        result=sigfillset(&blocked)?errno:pthread_attr_setsigmask_np(&attributes,&blocked);
        if(!result)result=pthread_create(&worker,&attributes,run,NULL);
        pthread_attr_destroy(&attributes);
        if(result){owner=0;errno=result;goto failed;}
        goto done;
failed:
        fprintf(stderr,"STEAMOHOS_SOCKET_QUERY op=start pid=%d errno=%d\n",getpid(),errno);
        if(listener>=0)close(listener);
        if(wake[0]>=0)close(wake[0]);
    if(wake[1]>=0)close(wake[1]);
    wake[0]=wake[1]=-1;
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
    int received=receive_record(channel,&response,sizeof(response),&fd,deadline);
    if(received!=1||response){error=received<0?errno:response?response:EPROTO;goto done;}
    if(request->version==1) {
        struct stat status;if(socket_stat(fd,request->inode,&status)){error=errno;goto done;}
    } else {
        struct EndpointPair remote={0};
        if(tcp_pair(fd,&remote)||!opposite_pair(pair,&remote)){error=EPROTO;goto done;}
    }
done:
    close(channel);
    if(error){
        static unsigned reports;
        if(__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<16)
            fprintf(stderr,"STEAMOHOS_SOCKET_QUERY op=query peer=%d descriptor=%d version=%u errno=%d\n",
                pid,request->descriptor,request->version,error);
        if(fd>=0)close(fd);
        errno=error;return -1;
    }
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

// HSL V3 contract adapted to VPP's bounded, one-request nonblocking channels.
int SteamOhosNextTcpSocket(pid_t pid,int minimum,struct SteamOhosSocketRecord *record) {
    if(!record||pid<=0||minimum<0){errno=EINVAL;return -1;}
    memset(record,0,sizeof(*record));
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(endpoint_path(pid,address.sun_path,sizeof(address.sun_path),0))return -1;
    int channel=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(channel<0)return -1;
    int fd=-1,error=0;int64_t deadline=milliseconds()+1000;
    if(connect(channel,(struct sockaddr*)&address,sizeof(address))&&errno!=EINPROGRESS){error=errno;goto done;}
    if(wait_io(channel,POLLOUT,deadline)||peer_is(channel,pid)){error=errno;goto done;}
    struct Request request={.version=3,.descriptor=minimum};
    if(transfer(channel,&request,sizeof(request),1,deadline)){error=errno;goto done;}
    int received=receive_record(channel,record,sizeof(*record),&fd,deadline);
    if(received<0){error=errno;goto done;}
    if(record->error<0||(record->error?received!=0:received!=1)){error=EPROTO;goto done;}
    if(record->error){error=record->error;goto done;}
    if(record->pid!=pid||record->uid!=geteuid()||record->descriptor<minimum||record->descriptor>=INT_MAX||
       record->ppid<0||record->pgid<0||!record->inode||!memchr(record->command,0,sizeof(record->command))) {error=EPROTO;goto done;}
    struct stat status;
    if(socket_stat(fd,record->inode,&status)||tcp_socket(fd)){error=errno;goto done;}
done:
    close(channel);if(error){if(fd>=0)close(fd);errno=error;return -1;}return fd;
}
int SteamOhosVisitTcpSockets(SteamOhosSocketVisitor visitor,void *context) {
    if(!visitor){errno=EINVAL;return -1;}
    char path[sizeof(endpoint)];if(endpoint_path(getpid(),path,sizeof(path),0))return -1;
    char *slash=strrchr(path,'/');if(!slash){errno=EINVAL;return -1;}*slash=0;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    DIR *directory=fdopendir(fd);if(!directory){int error=errno;close(fd);errno=error;return -1;}
    struct dirent *entry;unsigned peers=0,records=0;int64_t deadline=milliseconds()+2000;
    while(peers<128&&milliseconds()<deadline&&(entry=readdir(directory))) {
        if(!*entry->d_name||strspn(entry->d_name,"0123456789")!=strlen(entry->d_name))continue;
        char *end;long value=strtol(entry->d_name,&end,10);if(*end||value<=0||value>INT_MAX)continue;
        ++peers;int minimum=0;
        for(unsigned i=0;i<2048&&milliseconds()<deadline;++i) {
            struct SteamOhosSocketRecord record;int copy=SteamOhosNextTcpSocket((pid_t)value,minimum,&record);
            if(copy<0)break;
            minimum=record.descriptor+1;++records;int stop=visitor(&record,copy,context);close(copy);
            if(stop){closedir(directory);return (int)records;}
        }
    }
    closedir(directory);return (int)records;
}
__attribute__((visibility("default"))) void HslProcessRuntimeStopV1(void) {
    int saved=errno;pthread_mutex_lock(&lifecycle);pthread_mutex_lock(&guard);
    if(owner!=getpid()||listener<0){pthread_mutex_unlock(&guard);pthread_mutex_unlock(&lifecycle);errno=saved;return;}
    __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);
    if(wake[1]>=0){const char byte=1;ssize_t n=write(wake[1],&byte,1);(void)n;}
    pthread_mutex_unlock(&guard);
    pthread_join(worker,NULL);
    pthread_mutex_lock(&guard);
    close(listener);listener=-1;unlink(endpoint);owner=0;
    if(wake[0]>=0)close(wake[0]);
    if(wake[1]>=0)close(wake[1]);
    wake[0]=wake[1]=-1;
    pthread_mutex_unlock(&guard);pthread_mutex_unlock(&lifecycle);errno=saved;
}
__attribute__((destructor)) static void finalize(void) {HslProcessRuntimeStopV1();}
