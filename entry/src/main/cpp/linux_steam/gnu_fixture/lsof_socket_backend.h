/* OHOS app-private lsof backend. Original lsof selection/output stays in use.
   This header is compiled into the explicitly marked adapted GNU tool only. */
#include <dlfcn.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static void steamohos_lsof_trace(const char *stage,pid_t pid,int descriptor,uint64_t inode,int error) {
    static unsigned count;
    const char *enabled=getenv("STEAMOHOS_SOCKET_METADATA");
    if(enabled&&!strcmp(enabled,"1")&&count++<32)
        fprintf(stderr,"STEAMOHOS_LSOF_SOCKET stage=%s pid=%d fd=%d inode=%llu errno=%d\n",
            stage,pid,descriptor,(unsigned long long)inode,error);
}
static int steamohos_lsof_borrow(pid_t pid,int descriptor,uint64_t inode) {
    typedef int (*Borrow)(pid_t,int,uint64_t);
    const char *enabled=getenv("STEAMOHOS_SOCKET_METADATA");
    if(!enabled||strcmp(enabled,"1")){errno=ENOTSUP;return -1;}
    Borrow borrow=(Borrow)dlsym(RTLD_DEFAULT,"SteamOhosBorrowSocket");
    if(!borrow){errno=ENOSYS;return -1;}
    return borrow(pid,descriptor,inode);
}
static int steamohos_lsof_stat(pid_t pid,int descriptor,const char *path,struct stat *status) {
    char target[128],*end;ssize_t length=readlink(path,target,sizeof(target)-1);
    if(length<10||length>=(ssize_t)sizeof(target)-1)return 0;
    target[length]=0;if(strncmp(target,"socket:[",8))return 0;
    errno=0;unsigned long long inode=strtoull(target+8,&end,10);
    if(errno||end==target+8||end[0]!=']'||end[1]||!inode)return 0;
    int fd=steamohos_lsof_borrow(pid,descriptor,inode);
    if(fd<0){steamohos_lsof_trace("stat_borrow",pid,descriptor,inode,errno);return 0;}
    int result=fstat(fd,status),error=errno;close(fd);errno=error;
    steamohos_lsof_trace("stat",pid,descriptor,inode,result?error:0);
    return result==0;
}
static int steamohos_lsof_socket(struct stat *status,int valid) {
    if(!(valid&SB_INO))return 0;
    char *end;long descriptor=strtol(Lf->fd,&end,10);
    if(end==Lf->fd||*end||descriptor<0||descriptor>INT_MAX)return 0;
    int fd=steamohos_lsof_borrow(Lp->pid,(int)descriptor,(uint64_t)status->st_ino);
    if(fd<0){steamohos_lsof_trace("endpoint_borrow",Lp->pid,descriptor,status->st_ino,errno);return 0;}
    struct sockaddr_storage local,peer;socklen_t length=sizeof(local);
    int type;socklen_t type_length=sizeof(type);
    struct tcp_info information;socklen_t info_length=sizeof(information);
    if(getsockname(fd,(struct sockaddr*)&local,&length)||
       getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&type_length)||type!=SOCK_STREAM||
       getsockopt(fd,IPPROTO_TCP,TCP_INFO,&information,&info_length)||info_length<1) {
        steamohos_lsof_trace("endpoint_query",Lp->pid,descriptor,status->st_ino,errno);close(fd);return 0;}
    length=sizeof(peer);
    int connected=getpeername(fd,(struct sockaddr*)&peer,&length)==0;
    int peer_error=errno;close(fd);
    if(!connected&&peer_error!=ENOTCONN){steamohos_lsof_trace("peer_query",Lp->pid,descriptor,status->st_ino,peer_error);return 0;}
    steamohos_lsof_trace(connected?"connected":"listener",Lp->pid,descriptor,status->st_ino,0);
    int family=local.ss_family;
    if(family!=AF_INET
#if defined(HASIPv6)
       &&family!=AF_INET6
#endif
    )return 0;
    if(connected&&peer.ss_family!=family)return 0;
    int state=information.tcpi_state,index=state+TcpStOff;
    if(TcpStXn&&index>=0&&index<TcpNstates&&TcpStX[index]){Lf->sf|=SELEXCLF;return 1;}
    if(TcpStIn&&index>=0&&index<TcpNstates) {
        if(TcpStI[index])TcpStI[index]=2;else{Lf->sf|=SELEXCLF;return 1;}
    }
    if(Fnet&&((family==AF_INET&&FnetTy!=6)||(family==AF_INET6&&FnetTy!=4)))Lf->sf|=SELNET;
    snpf(Lf->type,sizeof(Lf->type),"%s",family==AF_INET?"IPv4":"IPv6");
    snpf(Lf->iproto,sizeof(Lf->iproto),"TCP");Lf->inp_ty=2;
    char inode_text[64];snpf(inode_text,sizeof(inode_text),InodeFmt_d,(INODETYPE)status->st_ino);
    enter_dev_ch(inode_text);Lf->inode=(INODETYPE)status->st_ino;
    if(family==AF_INET) {
        struct sockaddr_in *a=(struct sockaddr_in*)&local,*b=(struct sockaddr_in*)&peer;
        ent_inaddr((unsigned char*)&a->sin_addr,ntohs(a->sin_port),
            connected?(unsigned char*)&b->sin_addr:NULL,connected?ntohs(b->sin_port):0,family);
    }
#if defined(HASIPv6)
    else {
        struct sockaddr_in6 *a=(struct sockaddr_in6*)&local,*b=(struct sockaddr_in6*)&peer;
        ent_inaddr((unsigned char*)&a->sin6_addr,ntohs(a->sin6_port),
            connected?(unsigned char*)&b->sin6_addr:NULL,connected?ntohs(b->sin6_port):0,family);
    }
#endif
    Lf->lts.type=0;Lf->lts.state.i=state;
    // Queue/timer fields remain undefined when not queried from the kernel.
    return 1;
}
