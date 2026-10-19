#define _GNU_SOURCE 1
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
extern char**environ;
static void handler(int value){(void)value;}
static void *waiter(void *arg){char value;assert(read(*(int*)arg,&value,1)==1);return NULL;}
int main(int argc,char**argv){
 assert(argc==4);int mode=atoi(argv[3]);
 char *bad[]={"absent",NULL};assert(execve("/not-present-steamohos-exec",bad,environ)==-1&&errno==ENOENT);
 if(mode==7){
  int stop[2];assert(!pipe(stop));pthread_t thread;assert(!pthread_create(&thread,NULL,waiter,&stop[0]));
  assert(execve(argv[1],bad,environ)==-1&&errno==ENOTSUP);
  assert(write(stop[1],"S",1)==1);assert(!pthread_join(thread,NULL));
  close(stop[0]);close(stop[1]);puts("PASS multithread GNU exec rejected without damaging process");return 0;
 }
 if(mode==11){
  errno=0;assert(!popen("true","rx")&&errno==EINVAL);
  FILE *stream=popen("printf native-pipe; exit 37","re");assert(stream);
  assert(fcntl(fileno(stream),F_GETFD)&FD_CLOEXEC);
  char text[64]={0};assert(fread(text,1,sizeof(text)-1,stream)==11);
  assert(!strcmp(text,"native-pipe"));int status=pclose(stream);
  assert(WIFEXITED(status)&&WEXITSTATUS(status)==37);
  stream=popen("read value; test \"$value\" = pipe-input","w");assert(stream);
  assert(!(fcntl(fileno(stream),F_GETFD)&FD_CLOEXEC));
  assert(fputs("pipe-input\n",stream)>=0);assert(pclose(stream)==0);
  stream=popen("kill -TERM $$","r");assert(stream);status=pclose(stream);
  assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGTERM);
  int stop[2];assert(!pipe(stop));pthread_t thread;
  assert(!pthread_create(&thread,NULL,waiter,&stop[0]));
  stream=popen("printf threaded","r");assert(stream);
  memset(text,0,sizeof(text));assert(fread(text,1,sizeof(text)-1,stream)==8);
  assert(!strcmp(text,"threaded"));assert(pclose(stream)==0);
  assert(write(stop[1],"S",1)==1);assert(!pthread_join(thread,NULL));
  close(stop[0]);close(stop[1]);
  char shell[4096],backup[4096];snprintf(shell,sizeof(shell),"%s/steamohos_shell",argv[2]);
  snprintf(backup,sizeof(backup),"%s/shell-backup",argv[2]);assert(!rename(shell,backup));
  errno=0;assert(!popen("true","r")&&errno==ENOENT);assert(!rename(backup,shell));
  puts("PASS native popen read/write, CLOEXEC, exit/signal status, multithread caller, shell errors");return 0;
 }
 setenv("STEAMOHOS_EXEC_GENERATION","8",1);
 assert(execve(argv[1],bad,environ)==-1&&errno==ELOOP);
 unsetenv("STEAMOHOS_EXEC_GENERATION");
 char script[4096];snprintf(script,sizeof(script),"%s/script",argv[2]);
 assert(execve(script,bad,environ)==-1&&errno==ENOEXEC);
 int channel[2];assert(!pipe(channel));assert(write(channel[1],"T",1)==1);
 int closeOnExec=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(closeOnExec>=0);
 pid_t child=fork();assert(child>=0);
 if(!child){
  char pid[32],keep[32],closefd[32];snprintf(pid,sizeof(pid),"%d",(int)getpid());
  snprintf(keep,sizeof(keep),"%d",channel[0]);snprintf(closefd,sizeof(closefd),"%d",closeOnExec);
  char *args[]={"exec-target",pid,keep,closefd,NULL};
  assert(signal(SIGUSR1,handler)!=SIG_ERR);assert(signal(SIGUSR2,SIG_IGN)!=SIG_ERR);
  sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGTERM);assert(!sigprocmask(SIG_BLOCK,&mask,NULL));
  if(mode==0)execve(argv[1],args,environ);
  if(mode==1)execv(argv[1],args);
  if(mode==2)execl(argv[1],"exec-target",pid,keep,closefd,(char*)NULL);
  if(mode==3)execvpe("target",args,environ);
  if(mode==4)execlp("target","exec-target",pid,keep,closefd,(char*)NULL);
  if(mode==5)execle(argv[1],"exec-target",pid,keep,closefd,(char*)NULL,environ);
  if(mode==6)execvp("target",args);
  if(mode==8){assert(!chdir(argv[2]));execv("./sub/../target",args);}
  if(mode==9){
   char *shell[]={"/bin/sh","-c","exec -a exec-target \"$@\"","sh",argv[1],pid,keep,closefd,NULL};
   execve("/bin/sh",shell,environ);
  }
  if(mode==12){
   char wrapper[4096],reaper[4096];
   snprintf(wrapper,sizeof(wrapper),"%s/.steam/sdkarm64/steam-launch-wrapper",getenv("HOME"));
   snprintf(reaper,sizeof(reaper),"%s/.steam/sdkarm64/steam-launch-wrapper-other",getenv("HOME"));
   assert(execve(reaper,bad,environ)==-1&&errno==ENOENT);
   snprintf(reaper,sizeof(reaper),"%s/.steam/sdkx86_64/steam-launch-wrapper",getenv("HOME"));
   assert(execve(reaper,bad,environ)==-1&&errno==ENOENT);
   snprintf(reaper,sizeof(reaper),"%s/steamrtarm64/reaper",argv[2]);
   char *wrapped[]={"steam-launch-wrapper","--",reaper,argv[1],pid,keep,closefd,NULL};
   // Steam may supply a filtered environment. Private launch credentials must
   // survive only in the verified GNU child, without becoming game arguments.
   char *filtered[]={"EXEC_FIXTURE_MARKER=preserved",NULL};
   execve(wrapper,wrapped,filtered);
  }
  if(mode==10){
   char *affinity[]={"taskset","-c",getenv("EXEC_FIXTURE_CPU"),"/bin/sh","-c",
    "exec -a exec-target \"$@\"","sh",argv[1],pid,keep,closefd,NULL};
   execve("/system/bin/taskset",affinity,environ);
  }
  perror("native exec returned");_exit(81);
 }
 int status;assert(waitpid(child,&status,0)==child);
 if(!WIFEXITED(status)||WEXITSTATUS(status)){fprintf(stderr,"child_status=%d\n",status);return 82;}
 assert(fcntl(closeOnExec,F_GETFD)>=0);close(closeOnExec);close(channel[0]);close(channel[1]);return 0;
}
