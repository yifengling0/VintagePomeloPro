// Exercise live descriptors, FD reuse, peer identity, and normal-exit cleanup.
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(test) do { if (!(test)) { fprintf(stderr,"line=%d errno=%d: %s\n",__LINE__,errno,#test); exit(1); } } while(0)
static void (*observe)(int);
static int (*borrow)(pid_t,int,uint64_t);
static void (*stop)(void);
struct Record { int descriptor; uint64_t inode; };
static uint64_t inode(int fd) {struct stat s;REQUIRE(!fstat(fd,&s));return s.st_ino;}
static int bound(void) {
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);REQUIRE(fd>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    REQUIRE(!bind(fd,(struct sockaddr*)&a,sizeof(a)));observe(fd);return fd;
}
static int descriptors(void) {
    DIR *d=opendir("/proc/self/fd");REQUIRE(d);int count=0;struct dirent *e;
    while((e=readdir(d)))if(e->d_name[0]!='.')++count;
    closedir(d);return count;
}
static int query_channel(pid_t pid) {
    const char *root=getenv("STEAMOHOS_SOCKET_METADATA_ROOT");
    if(!root)root=getenv("STEAMOHOS_IPC_ROOT");
    struct sockaddr_un path={.sun_family=AF_UNIX};
    REQUIRE(root&&snprintf(path.sun_path,sizeof(path.sun_path),"%s/socket-metadata/%d",root,pid)<(int)sizeof(path.sun_path));
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);REQUIRE(fd>=0);
    REQUIRE(!connect(fd,(struct sockaddr*)&path,sizeof(path)));
    return fd;
}
static void fragmented_query(pid_t pid,int descriptor,uint64_t expected) {
    int channel=query_channel(pid);
    struct {uint32_t version;int32_t descriptor;uint64_t inode;} request={1,descriptor,expected};
    REQUIRE(send(channel,&request,4,MSG_NOSIGNAL)==4);
    // A scheduling pause between stream fragments must not be misclassified
    // as an invalid peer or stale socket. This failed with the old 150ms read.
    usleep(250000);
    REQUIRE(send(channel,(char*)&request+4,sizeof(request)-4,MSG_NOSIGNAL)==sizeof(request)-4);
    int response=-1;struct iovec io={.iov_base=&response,.iov_len=sizeof(response)};
    union {struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} ancillary={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=sizeof(ancillary.bytes)};
    struct timeval timeout={2,0};REQUIRE(!setsockopt(channel,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    REQUIRE(recvmsg(channel,&message,MSG_CMSG_CLOEXEC|MSG_WAITALL)==sizeof(response)&&!response);
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);REQUIRE(header&&header->cmsg_level==SOL_SOCKET&&header->cmsg_type==SCM_RIGHTS);
    int fd;memcpy(&fd,CMSG_DATA(header),sizeof(fd));REQUIRE(inode(fd)==expected&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));close(fd);close(channel);
}
static void rejected(pid_t pid,int fd,uint64_t expected,int error) {
    errno=0;int result=borrow(pid,fd,expected);int actual=errno;
    if(result>=0)close(result);
    if(result!=-1||(actual!=error&&!(error==EBADF&&actual==ESTALE)))
        fprintf(stderr,"reject pid=%d fd=%d expected_errno=%d actual_errno=%d result=%d\n",pid,fd,error,actual,result);
    // The broker's accepted Unix channel may reuse an already closed FD.
    REQUIRE(result==-1&&(actual==error||(error==EBADF&&actual==ESTALE)));
}
int main(void) {
    observe=(void(*)(int))dlsym(RTLD_DEFAULT,"SteamOhosObserveSocket");
    borrow=(int(*)(pid_t,int,uint64_t))dlsym(RTLD_DEFAULT,"SteamOhosBorrowSocket");
    stop=(void(*)(void))dlsym(RTLD_DEFAULT,"HslProcessRuntimeStopV1");
    REQUIRE(observe&&borrow&&stop);
    const int initial_descriptors=descriptors();
    int own=bound(),commands[2],responses[2];REQUIRE(!pipe2(commands,O_CLOEXEC)&&!pipe2(responses,O_CLOEXEC));
    pid_t child=fork();REQUIRE(child>=0);
    if(!child) {
        close(own);close(commands[1]);close(responses[0]);
        int fd=bound();struct Record record={.descriptor=fd,.inode=inode(fd)};
        REQUIRE(write(responses[1],&record,sizeof(record))==sizeof(record));
        char action;
        while(read(commands[0],&action,1)==1) {
            if(action=='C'){REQUIRE(!close(fd));char reply='C';REQUIRE(write(responses[1],&reply,1)==1);}
            else if(action=='R') {
                int replacement=bound();
                if(replacement!=fd){REQUIRE(dup2(replacement,fd)==fd);close(replacement);}
                record.inode=inode(fd);REQUIRE(write(responses[1],&record,sizeof(record))==sizeof(record));
            } else if(action=='X'){close(fd);close(commands[0]);close(responses[1]);exit(0);}
            else exit(2);
        }
        exit(3);
    }
    close(commands[0]);close(responses[1]);struct Record initial,current;
    REQUIRE(read(responses[0],&initial,sizeof(initial))==sizeof(initial));
    fragmented_query(child,initial.descriptor,initial.inode);
    int baseline=descriptors();
    for(int i=0;i<512;++i) {
        int fd=borrow(child,initial.descriptor,initial.inode);REQUIRE(fd>=0);
        REQUIRE(inode(fd)==initial.inode&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));close(fd);
    }
    REQUIRE(descriptors()==baseline);
    // An accepted but incomplete query cannot occupy the broker forever.
    int abandoned=query_channel(child);REQUIRE(send(abandoned,"x",1,MSG_NOSIGNAL)==1);
    struct timespec began,ended;REQUIRE(!clock_gettime(CLOCK_MONOTONIC,&began));
    int delayed=borrow(child,initial.descriptor,initial.inode);
    int delayed_error=errno;
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC,&ended));
    long elapsed=(ended.tv_sec-began.tv_sec)*1000+(ended.tv_nsec-began.tv_nsec)/1000000;
    if(delayed<0||elapsed>=300)fprintf(stderr,"head_of_line elapsed=%ld error=%d\n",elapsed,delayed_error);
    REQUIRE(delayed>=0&&elapsed<300);
    REQUIRE(inode(delayed)==initial.inode);close(delayed);
    long independent_elapsed=elapsed;
    close(abandoned);poll(NULL,0,30);
    delayed=borrow(child,initial.descriptor,initial.inode);REQUIRE(delayed>=0&&inode(delayed)==initial.inode);close(delayed);
    REQUIRE(descriptors()==baseline);
    // Several slow or abandoned readers must not delay an independent query.
    int stalled[8];
    for(unsigned i=0;i<8;++i){stalled[i]=query_channel(child);REQUIRE(send(stalled[i],"x",1,MSG_NOSIGNAL)==1);}
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC,&began));
    delayed=borrow(child,initial.descriptor,initial.inode);
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC,&ended));
    elapsed=(ended.tv_sec-began.tv_sec)*1000+(ended.tv_nsec-began.tv_nsec)/1000000;
    REQUIRE(delayed>=0&&inode(delayed)==initial.inode&&elapsed<300);close(delayed);
    for(unsigned i=0;i<8;++i)close(stalled[i]);
    poll(NULL,0,80);REQUIRE(descriptors()==baseline);
    rejected(child,initial.descriptor,initial.inode+1,ESTALE);
    REQUIRE(write(commands[1],"C",1)==1);char reply;REQUIRE(read(responses[0],&reply,1)==1&&reply=='C');
    rejected(child,initial.descriptor,initial.inode,EBADF);
    REQUIRE(write(commands[1],"R",1)==1);REQUIRE(read(responses[0],&current,sizeof(current))==sizeof(current));
    REQUIRE(current.descriptor==initial.descriptor&&current.inode!=initial.inode);
    rejected(child,current.descriptor,initial.inode,ESTALE);
    int fd=borrow(child,current.descriptor,current.inode);REQUIRE(fd>=0&&inode(fd)==current.inode);close(fd);
    char directory[256],path[256],own_path[256];const char *root=getenv("STEAMOHOS_SOCKET_METADATA_ROOT");
    if(!root)root=getenv("STEAMOHOS_IPC_ROOT");
    REQUIRE(root);
    REQUIRE(snprintf(directory,sizeof(directory),"%s/socket-metadata",root)<(int)sizeof(directory));
    REQUIRE(!chmod(directory,0770));rejected(child,current.descriptor,current.inode,EACCES);REQUIRE(!chmod(directory,0700));
    REQUIRE(write(commands[1],"X",1)==1);int status;REQUIRE(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    REQUIRE(snprintf(path,sizeof(path),"%s/%d",directory,child)<(int)sizeof(path));
    struct stat s;REQUIRE(lstat(path,&s)==-1&&errno==ENOENT);
    // A same-UID socket with the wrong kernel PID must not impersonate a peer.
    REQUIRE(snprintf(own_path,sizeof(own_path),"%s/%d",directory,getpid())<(int)sizeof(own_path));
    REQUIRE(!symlink(own_path,path));rejected(child,own,inode(own),EACCES);REQUIRE(!unlink(path));
    // Authentication can reject before the worker has even accepted the
    // connection. A transient baseline followed by accept is legitimate, so
    // polling the count cannot establish that the worker is quiescent.
    // Join the owned worker, then check every descriptor against the count
    // before it was created. This also verifies terminal shutdown with the
    // rejected query still queued or accepted, without weakening leak checks.
    stop();stop();
    REQUIRE(fcntl(own,F_GETFD)>=0);
    REQUIRE(lstat(own_path,&s)==-1&&errno==ENOENT);
    close(own);close(commands[1]);close(responses[0]);
    REQUIRE(descriptors()==initial_descriptors);
    printf("SOCKET_METADATA independent_latency_ms=%ld stalled8_latency_ms=%ld\n",independent_elapsed,elapsed);
    puts("SOCKET_METADATA lifecycle verified=1 queries=512 fragmented_query=1 fd_leaks=0 bounded_abandoned=1 independent_queries=1 closed_rejected=1 reused_rejected=1 wrong_peer_rejected=1 exit_cleanup=1 terminal_shutdown=1");
    return 0;
}
