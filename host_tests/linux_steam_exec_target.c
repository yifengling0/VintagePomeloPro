#define _GNU_SOURCE 1
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
int main(int argc,char**argv){
 if(getenv("EXEC_FIXTURE_RETIRED")){
  void *address=(void*)strtoul(getenv("EXEC_FIXTURE_RETIRED"),NULL,16);unsigned char residency;
  errno=0;assert(mincore(address,4096,&residency)==-1&&errno==ENOMEM);
  // A reclaimed inherited heap must not break the fresh libc allocator.
  for(size_t i=1;i<1000;++i){void *memory=malloc(i*257);assert(memory);memset(memory,0xa5,i*257);free(memory);}
  puts("PASS inherited 64 MiB mapping reclaimed; fresh allocator usable");
 }
 assert(argc==4 && !strcmp(argv[0],"exec-target"));
 assert(getpid()==atoi(argv[1]));
 char value=0;assert(read(atoi(argv[2]),&value,1)==1 && value=='T');
 assert(fcntl(atoi(argv[3]),F_GETFD)==-1 && errno==EBADF);
 assert(!strcmp(getenv("EXEC_FIXTURE_MARKER"),"preserved"));
 assert(getenv("STEAMOHOS_SOCKET_METADATA_ROOT")&&
        !strcmp(getenv("STEAMOHOS_SOCKET_METADATA_ROOT"),"/var/tmp/steamohos-exec-scope"));
 assert(getenv("STEAMOHOS_SYSVIPC_ROOT")&&
        !strcmp(getenv("STEAMOHOS_SYSVIPC_ROOT"),"/var/tmp/steamohos-exec-sysvipc"));
 assert(!strcmp(getenv("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET"),"/var/tmp/exec-fixture-audio"));
 assert(!strcmp(getenv("ALSA_CONFIG_PATH"),"/var/tmp/exec-fixture-asound.conf"));
 assert(!strcmp(getenv("SDL_AUDIO_DRIVER"),"alsa"));
 assert(!strcmp(getenv("STEAMOHOS_GAMEPAD_SOCKET"),"/var/tmp/exec-fixture-gamepad"));
 assert(!strcmp(getenv("EXEC_FIXTURE_GAMEPAD_BRIDGE"),"present"));
 struct sigaction state;assert(!sigaction(SIGUSR1,NULL,&state)&&state.sa_handler==SIG_DFL);
 assert(!sigaction(SIGUSR2,NULL,&state)&&state.sa_handler==SIG_IGN);
 sigset_t mask;assert(!sigprocmask(SIG_SETMASK,NULL,&mask)&&sigismember(&mask,SIGTERM)==1);
 if(getenv("EXEC_FIXTURE_CPU")){
  cpu_set_t cpus;CPU_ZERO(&cpus);assert(!sched_getaffinity(0,sizeof(cpus),&cpus));
  assert(CPU_COUNT(&cpus)==1&&CPU_ISSET(atoi(getenv("EXEC_FIXTURE_CPU")),&cpus));
 }
 puts("PASS GNU native exec: PID/argv/environment/non-CLOEXEC fd preserved; CLOEXEC fd closed");
 return 0;
}
