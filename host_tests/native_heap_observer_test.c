#define _GNU_SOURCE 1
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <sys/wait.h>
#include "../scripts/diagnostics/native_heap_observer.h"
static _Atomic int release_workers;
static void* worker(void* value) {while(!atomic_load(&release_workers))usleep(1000);return value;}
static unsigned threads(void) {
 DIR* d=opendir("/proc/self/task");assert(d);unsigned count=0;
 for(struct dirent* p;(p=readdir(d));)if(p->d_name[0]!='.')++count;
 closedir(d);return count;
}
int main(void) {
 setenv("STEAMOHOS_EXEC_PATH","/fixture/steamwebhelper",1);
 assert(threads()==1);pthread_t handles[16];
 for(unsigned i=0;i<16;++i){assert(pthread_create(&handles[i],NULL,worker,(void*)(uintptr_t)(i+1))==0);if(i<11)assert(threads()==i+2);}
 for(unsigned i=0;i<100&&threads()!=18;++i)usleep(1000);
 assert(threads()==18);pid_t child=fork();assert(child>=0);
 if(!child){assert(threads()==1);_exit(0);}
 int status;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
 atomic_store(&release_workers,1);
 for(unsigned i=0;i<16;++i){void* out=NULL;assert(pthread_join(handles[i],&out)==0);assert(out==(void*)(uintptr_t)(i+1));}
 puts("Heap observer: startup single-threaded; original thread results and fork semantics preserved");return 0;
}
