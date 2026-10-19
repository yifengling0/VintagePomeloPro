// Independent native GNU capability probe. Only its own loopback pair is used.
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef STEAMOHOS_LSOF_PROGRAM
#define STEAMOHOS_LSOF_PROGRAM "/bin/lsof"
#endif

static void descriptor(pid_t pid,int fd,const char *role) {
    char path[128],target[256];struct stat direct,through,raw,opened;
    snprintf(path,sizeof(path),"/proc/%d/fd/%d",pid,fd);
    ssize_t n=readlink(path,target,sizeof(target)-1);int link_error=n<0?errno:0;
    if(n>=0)target[n]=0;else target[0]=0;
    int a=pid==getpid()?fstat(fd,&direct):-2,a_error=a==-1?errno:0;
    int b=stat(path,&through),b_error=b?errno:0;
    int c=pid==getpid()?(int)syscall(SYS_fstat,fd,&raw):-2,c_error=c==-1?errno:0;
    int pathfd=open(path,O_PATH|O_CLOEXEC),path_error=pathfd<0?errno:0;
    int d=pathfd>=0?(int)syscall(SYS_fstat,pathfd,&opened):-2,d_error=d==-1?errno:0;
    if(pathfd>=0)close(pathfd);
    fprintf(stderr,"CONNECTION_IDENTITY descriptor role=%s pid=%d fd=%d link_errno=%d target=%s fstat=%d/%d stat=%d/%d raw_fstat=%d/%d path_open=%d/%d path_fstat=%d/%d mode=%o inode=%" PRIuMAX "\n",
        role,pid,fd,link_error,target,a,a_error,b,b_error,c,c_error,pathfd,path_error,d,d_error,
        !b?(unsigned)through.st_mode:!a?(unsigned)direct.st_mode:!c?(unsigned)raw.st_mode:!d?(unsigned)opened.st_mode:0,
        !b?(uintmax_t)through.st_ino:!a?(uintmax_t)direct.st_ino:!c?(uintmax_t)raw.st_ino:!d?(uintmax_t)opened.st_ino:0);
}

static void socket_diagnostic(uint16_t port) {
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_SOCK_DIAG);
    fprintf(stderr,"CONNECTION_IDENTITY netlink_socket=%d errno=%d\n",fd,fd<0?errno:0);
    if(fd<0)return;
    struct {struct nlmsghdr header;struct inet_diag_req_v2 request;} query={0};
    query.header.nlmsg_len=sizeof(query);query.header.nlmsg_type=SOCK_DIAG_BY_FAMILY;
    query.header.nlmsg_flags=NLM_F_REQUEST|NLM_F_DUMP;query.header.nlmsg_seq=1;
    query.request.sdiag_family=AF_INET;query.request.sdiag_protocol=IPPROTO_TCP;
    query.request.idiag_states=~0U;
    struct sockaddr_nl kernel={.nl_family=AF_NETLINK};
    ssize_t sent=sendto(fd,&query,sizeof(query),0,(struct sockaddr*)&kernel,sizeof(kernel));
    fprintf(stderr,"CONNECTION_IDENTITY netlink_send=%zd errno=%d\n",sent,sent<0?errno:0);
    if(sent<0){close(fd);return;}
    unsigned matches=0;
    for(unsigned i=0;i<64;++i) {
        struct pollfd ready={.fd=fd,.events=POLLIN};
        if(poll(&ready,1,250)<=0){fprintf(stderr,"CONNECTION_IDENTITY netlink_timeout\n");break;}
        unsigned char bytes[32768];ssize_t n=recv(fd,bytes,sizeof(bytes),0);
        if(n<=0){fprintf(stderr,"CONNECTION_IDENTITY netlink_recv=%zd errno=%d\n",n,n<0?errno:0);break;}
        int remaining=(int)n;
        for(struct nlmsghdr *h=(struct nlmsghdr*)bytes;NLMSG_OK(h,remaining);h=NLMSG_NEXT(h,remaining)) {
            if(h->nlmsg_type==NLMSG_DONE){close(fd);fprintf(stderr,"CONNECTION_IDENTITY netlink_done matches=%u\n",matches);return;}
            if(h->nlmsg_type==NLMSG_ERROR&&h->nlmsg_len>=NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                const struct nlmsgerr *e=NLMSG_DATA(h);
                fprintf(stderr,"CONNECTION_IDENTITY netlink_error=%d\n",e->error);close(fd);return;
            }
            if(h->nlmsg_type!=SOCK_DIAG_BY_FAMILY||h->nlmsg_len<NLMSG_LENGTH(sizeof(struct inet_diag_msg)))continue;
            const struct inet_diag_msg *message=NLMSG_DATA(h);
            if(ntohs(message->id.idiag_sport)!=port&&ntohs(message->id.idiag_dport)!=port)continue;
            if(matches++<8)fprintf(stderr,"CONNECTION_IDENTITY netlink_row inode=%u uid=%u state=%u source_port=%u destination_port=%u\n",
                message->idiag_inode,message->idiag_uid,message->idiag_state,ntohs(message->id.idiag_sport),ntohs(message->id.idiag_dport));
        }
    }
    close(fd);
}

static void table(const char *path,uint16_t port) {
    FILE *f=fopen(path,"r");int error=f?0:errno;
    fprintf(stderr,"CONNECTION_IDENTITY table path=%s open_errno=%d\n",path,error);
    if(!f)return;
    char line[1024],needle[16];snprintf(needle,sizeof(needle),":%04X",port);
    unsigned lines=0,matches=0;
    while(fgets(line,sizeof(line),f)&&lines++<16384) {
        if(lines==1)fprintf(stderr,"CONNECTION_IDENTITY header %s",line);
        if(strstr(line,needle)&&matches++<8)fprintf(stderr,"CONNECTION_IDENTITY row %s",line);
    }
    fprintf(stderr,"CONNECTION_IDENTITY table_done lines=%u matches=%u read_error=%d\n",lines,matches,ferror(f));
    fclose(f);
}

static int send_descriptor(int channel,int fd) {
    char byte='S';struct iovec io={.iov_base=&byte,.iov_len=1};
    union {struct cmsghdr aligned;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
    header->cmsg_len=CMSG_LEN(sizeof(fd));memcpy(CMSG_DATA(header),&fd,sizeof(fd));
    return sendmsg(channel,&message,MSG_NOSIGNAL)==1?0:-1;
}

static void received_descriptor(int channel) {
    char byte=0;struct iovec io={.iov_base=&byte,.iov_len=1};
    union {struct cmsghdr aligned;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control={0};
    struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t count=recvmsg(channel,&message,MSG_CMSG_CLOEXEC);int error=count<0?errno:0,fd=-1;
    struct cmsghdr *header=CMSG_FIRSTHDR(&message);
    if(count==1&&!(message.msg_flags&(MSG_CTRUNC|MSG_TRUNC))&&header&&
       header->cmsg_level==SOL_SOCKET&&header->cmsg_type==SCM_RIGHTS&&header->cmsg_len==CMSG_LEN(sizeof(int)))
        memcpy(&fd,CMSG_DATA(header),sizeof(fd));
    fprintf(stderr,"CONNECTION_IDENTITY receive_fd count=%zd errno=%d borrowed_fd=%d\n",count,error,fd);
    if(fd<0)return;
    struct sockaddr_in local={0},peer={0};socklen_t size=sizeof(local);
    int a=getsockname(fd,(struct sockaddr*)&local,&size),ae=a?errno:0;size=sizeof(peer);
    int b=getpeername(fd,(struct sockaddr*)&peer,&size),be=b?errno:0;
    fprintf(stderr,"CONNECTION_IDENTITY borrowed_endpoints local=%d/%d port=%u peer=%d/%d port=%u cloexec=%d\n",
        a,ae,ntohs(local.sin_port),b,be,ntohs(peer.sin_port),!!(fcntl(fd,F_GETFD)&FD_CLOEXEC));
    descriptor(getpid(),fd,"borrowed-child-client");close(fd);
}

static void observe_descriptor(int fd) {
    void (*observe)(int)=(void(*)(int))dlsym(RTLD_DEFAULT,"SteamOhosObserveSocket");
    if(observe)observe(fd);
}
static void live_query(pid_t child,int descriptor_number) {
    typedef int (*Borrow)(pid_t,int,uint64_t);
    Borrow borrow=(Borrow)dlsym(RTLD_DEFAULT,"SteamOhosBorrowSocket");
    if(!borrow)return;
    char path[128],target[128];snprintf(path,sizeof(path),"/proc/%d/fd/%d",child,descriptor_number);
    ssize_t length=readlink(path,target,sizeof(target)-1);
    if(length<0)return;
    target[length]=0;unsigned long long inode=0;char extra=0;
    if(sscanf(target,"socket:[%llu]%c",&inode,&extra)!=1)return;
    int fd=borrow(child,descriptor_number,inode),error=fd<0?errno:0;
    fprintf(stderr,"CONNECTION_IDENTITY live_query child=%d descriptor=%d borrowed=%d errno=%d\n",child,descriptor_number,fd,error);
    if(fd>=0){descriptor(getpid(),fd,"live-query-child-client");close(fd);}
    int stale=borrow(child,descriptor_number,inode+1),stale_error=stale<0?errno:0;
    if(stale>=0)close(stale);
    fprintf(stderr,"CONNECTION_IDENTITY stale_query fd=%d errno=%d\n",stale,stale_error);
}

int main(void) {
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(listener<0){perror("identity socket");return 2;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    socklen_t length=sizeof(address);
    if(bind(listener,(struct sockaddr*)&address,length)||listen(listener,1)||
       getsockname(listener,(struct sockaddr*)&address,&length)){perror("identity listen");close(listener);return 2;}
    observe_descriptor(listener);
    int control[2],ready[2],transfer[2];
    if(pipe2(control,O_CLOEXEC)||pipe2(ready,O_CLOEXEC)||socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,transfer)){
        perror("identity channels");close(listener);return 2;}
    pid_t child=fork();
    if(child<0){perror("identity fork");return 2;}
    if(!child) {
        close(control[1]);close(ready[0]);close(transfer[0]);close(listener);
        int client=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(client<0||connect(client,(struct sockaddr*)&address,sizeof(address)))_exit(3);
        observe_descriptor(client);
        descriptor(getpid(),client,"client");
        if(write(ready[1],&client,sizeof(client))!=sizeof(client))_exit(3);
        if(send_descriptor(transfer[1],client))_exit(3);
        close(transfer[1]);
        close(ready[1]);char command;
        while(read(control[0],&command,1)<0&&errno==EINTR){}
        close(client);close(control[0]);_exit(0);
    }
    close(control[0]);close(ready[1]);close(transfer[1]);
    int client=-1;
    ssize_t received=read(ready[0],&client,sizeof(client));close(ready[0]);
    if(received!=sizeof(client)){close(control[1]);close(listener);waitpid(child,NULL,0);return 3;}
    int accepted=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
    if(accepted<0){close(control[1]);close(listener);waitpid(child,NULL,0);return 3;}
    fprintf(stderr,"CONNECTION_IDENTITY pair parent=%d child=%d port=%u\n",getpid(),child,ntohs(address.sin_port));
    descriptor(getpid(),listener,"listener");descriptor(getpid(),accepted,"accepted");descriptor(child,client,"child-client");
    received_descriptor(transfer[0]);close(transfer[0]);
    live_query(child,client);
    table("/proc/net/tcp",ntohs(address.sin_port));table("/proc/self/net/tcp",ntohs(address.sin_port));
    char child_table[128];snprintf(child_table,sizeof(child_table),"/proc/%d/net/tcp",child);
    table(child_table,ntohs(address.sin_port));
    socket_diagnostic(ntohs(address.sin_port));
    char command[4096];snprintf(command,sizeof(command),"'%s' -P -F upnR -i TCP@127.0.0.1:%u",STEAMOHOS_LSOF_PROGRAM,ntohs(address.sin_port));
    FILE *output=popen(command,"r");int status=-1,parent_found=0,child_found=0;
    if(!output)fprintf(stderr,"CONNECTION_IDENTITY popen_errno=%d\n",errno);
    else {
        char line[1024],parent_record[48],child_record[48];
        snprintf(parent_record,sizeof(parent_record),"p%d\n",getpid());snprintf(child_record,sizeof(child_record),"p%d\n",child);
        unsigned lines=0;
        while(fgets(line,sizeof(line),output)) {
            if(!strcmp(line,parent_record))parent_found=1;
            if(!strcmp(line,child_record))child_found=1;
            if(lines++<64)fprintf(stderr,"CONNECTION_IDENTITY lsof %s",line);
        }
        status=pclose(output);
    }
    close(control[1]);close(accepted);close(listener);
    int child_status;while(waitpid(child,&child_status,0)<0){if(errno!=EINTR)return 4;}
    int verified=status>=0&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&parent_found&&child_found&&
        WIFEXITED(child_status)&&WEXITSTATUS(child_status)==0;
    fprintf(stderr,"CONNECTION_IDENTITY result verified=%d lsof_status=%d parent_found=%d child_found=%d child_status=%d\n",
        verified,status,parent_found,child_found,child_status);
    return verified?0:1;
}
