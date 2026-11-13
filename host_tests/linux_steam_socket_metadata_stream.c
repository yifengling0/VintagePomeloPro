// A genuine same-PID peer sends SCM_RIGHTS with a fragmented response.
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line=%d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
int main(void) {
 int (*borrow)(pid_t,int,uint64_t)=dlsym(RTLD_DEFAULT,"SteamOhosBorrowSocket");CHECK(borrow);
 int ready[2];CHECK(!pipe2(ready,O_CLOEXEC));pid_t child=fork();CHECK(child>=0);
 if(!child) {
  close(ready[0]);const char *root=getenv("STEAMOHOS_IPC_ROOT");CHECK(root);
  char directory[256];CHECK(snprintf(directory,sizeof(directory),"%s/socket-metadata",root)<(int)sizeof(directory));CHECK(!mkdir(directory,0700));
  struct sockaddr_un path={.sun_family=AF_UNIX};CHECK(snprintf(path.sun_path,sizeof(path.sun_path),"%s/%d",directory,getpid())<(int)sizeof(path.sun_path));
  int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);CHECK(server>=0&&!bind(server,(struct sockaddr*)&path,sizeof(path))&&!listen(server,4));
  int tcp=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);CHECK(tcp>=0);struct stat status;CHECK(!fstat(tcp,&status));
  struct {int fd;uint64_t inode;} record={tcp,status.st_ino};CHECK(write(ready[1],&record,sizeof(record))==sizeof(record));
  for(int test=0;test<3;++test) {
   int peer=accept4(server,NULL,NULL,SOCK_CLOEXEC);CHECK(peer>=0);
   char request[16];CHECK(recv(peer,request,sizeof(request),MSG_WAITALL)==sizeof(request));
   int response=0;struct iovec io={&response,1};
   union {struct cmsghdr align;char bytes[CMSG_SPACE(2*sizeof(int))];} ancillary={0};
   unsigned count=test==1?2:1;
   struct msghdr message={.msg_iov=&io,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=CMSG_SPACE(count*sizeof(int))};
   struct cmsghdr *header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(count*sizeof(int));
   for(unsigned n=0;n<count;++n)memcpy((char*)CMSG_DATA(header)+n*sizeof(int),&tcp,sizeof(tcp));
   CHECK(sendmsg(peer,&message,MSG_NOSIGNAL)==1);
   if(test!=2){usleep(250000);ssize_t sent=send(peer,(char*)&response+1,sizeof(response)-1,MSG_NOSIGNAL);CHECK(sent==3||(test==1&&sent<0&&errno==EPIPE));}
   close(peer);
  }
  close(tcp);close(server);unlink(path.sun_path);close(ready[1]);exit(0);
 }
 close(ready[1]);struct {int fd;uint64_t inode;} record;CHECK(read(ready[0],&record,sizeof(record))==sizeof(record));
 int fd=borrow(child,record.fd,record.inode);CHECK(fd>=0);struct stat status;CHECK(!fstat(fd,&status)&&status.st_ino==record.inode&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));close(fd);
 errno=0;CHECK(borrow(child,record.fd,record.inode)==-1&&errno==EPROTO);
 errno=0;CHECK(borrow(child,record.fd,record.inode)==-1&&errno==EPROTO);
 int result;CHECK(waitpid(child,&result,0)==child&&WIFEXITED(result)&&!WEXITSTATUS(result));close(ready[0]);
 puts("SOCKET_METADATA fragmented_response=1 duplicate_fd_rejected=1 incomplete_response_rejected=1");return 0;
}
