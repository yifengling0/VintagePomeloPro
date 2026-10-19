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
#include <unistd.h>

#define REQUIRE(test) do { if (!(test)) { fprintf(stderr,"line=%d errno=%d: %s\n",__LINE__,errno,#test); exit(1); } } while(0)
static void (*observe)(int);
static int (*borrow)(pid_t,int,uint64_t);
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
    REQUIRE(observe&&borrow);
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
    int baseline=descriptors();
    for(int i=0;i<512;++i) {
        int fd=borrow(child,initial.descriptor,initial.inode);REQUIRE(fd>=0);
        REQUIRE(inode(fd)==initial.inode&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));close(fd);
    }
    REQUIRE(descriptors()==baseline);
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
    REQUIRE(descriptors()==baseline);close(own);close(commands[1]);close(responses[0]);
    puts("SOCKET_METADATA lifecycle verified=1 queries=512 fd_leaks=0 closed_rejected=1 reused_rejected=1 wrong_peer_rejected=1 exit_cleanup=1");
    return 0;
}
