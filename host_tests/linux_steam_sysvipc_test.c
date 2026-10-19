#define _GNU_SOURCE 1
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <pthread.h>
#include <fcntl.h>
union Arg {int val;struct semid_ds *buf;unsigned short *array;};
int main(void){
    const char *root=getenv("STEAMOHOS_IPC_ROOT");
    if(root){
        char linkpath[4096];snprintf(linkpath,sizeof(linkpath),"%s/steamohos-symlink-probe-%d",root,getpid());
        int result=symlink("steamohos-target",linkpath),error=errno;
        fprintf(stderr,"STEAMOHOS_PRIVATE_SYMLINK supported=%d errno=%d\n",result==0,result?error:0);
        if(!result){char content[80];ssize_t n=readlink(linkpath,content,sizeof(content));assert(n==(ssize_t)strlen("steamohos-target")&&!memcmp(content,"steamohos-target",(size_t)n));assert(!unlink(linkpath));}
    }
    const key_t key=(key_t)(0x12340000u | (getpid()&65535));
    int id=semget(key,2,IPC_CREAT|IPC_EXCL|0600);assert(id>=0);
    assert(semget(key,0,0600)==id);
    assert(semget(key,2,IPC_CREAT|IPC_EXCL|0600)==-1&&errno==EEXIST);
    union Arg argument={.val=1};assert(!semctl(id,0,SETVAL,argument));
    struct sembuf atomic[]={{0,-1,IPC_NOWAIT},{1,-1,IPC_NOWAIT}};
    assert(semop(id,atomic,2)==-1&&errno==EAGAIN);assert(semctl(id,0,GETVAL)==1);
    int ready[2];assert(!pipe(ready));pid_t child=fork();assert(child>=0);
    if(!child){close(ready[0]);struct sembuf take={0,-1,SEM_UNDO};assert(!semop(id,&take,1));assert(write(ready[1],"X",1)==1);for(;;)pause();}
    close(ready[1]);char marker;assert(read(ready[0],&marker,1)==1);close(ready[0]);assert(semctl(id,0,GETVAL)==0);
    assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child);
    assert(semctl(id,0,GETVAL)==1);
    struct sembuf blocked={1,-1,0};struct timespec deadline={0,30000000};
    assert(semtimedop(id,&blocked,1,&deadline)==-1&&errno==EAGAIN);
    struct semid_ds info;argument.buf=&info;assert(!semctl(id,0,IPC_STAT,argument)&&info.sem_nsems==2);
    assert(!semctl(id,0,IPC_RMID));assert(semget(key,0,0600)==-1&&errno==ENOENT);
    assert(semctl(id,0,GETVAL)==-1&&errno==EINVAL);
    int memory=shmget(key,4096,IPC_CREAT|IPC_EXCL|0600);assert(memory>=0);
    char *data=shmat(memory,NULL,0);assert(data!=(void*)-1);strcpy(data,"parent");
    child=fork();assert(child>=0);
    if(!child){char *other=shmat(memory,NULL,0);assert(other!=(void*)-1&&!strcmp(other,"parent"));strcpy(other,"child");assert(!shmdt(other));_exit(0);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(!strcmp(data,"child"));
    struct shmid_ds shminfo;assert(!shmctl(memory,IPC_STAT,&shminfo)&&shminfo.shm_segsz==4096);
    assert(!shmctl(memory,IPC_RMID,NULL));assert(!strcmp(data,"child"));assert(!shmdt(data));
    assert(shmget(key,0,0600)==-1&&errno==ENOENT);
    struct robust_list_head *head=NULL;size_t head_size=0;
    assert(!syscall(SYS_get_robust_list,0,&head,&head_size));
    assert(head&&head_size==sizeof(*head));
    char name[100];snprintf(name,sizeof(name),"/steamohos-robust-%d",getpid());
    int fd=shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);
    assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    assert(shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600)==-1&&errno==EEXIST);
    assert(!ftruncate(fd,4096));
    pthread_mutex_t *mutex=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(mutex!=MAP_FAILED);
    pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED));
    assert(!pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST));
    assert(!pthread_mutex_init(mutex,&attr));assert(!pthread_mutexattr_destroy(&attr));
    assert(!pipe(ready));child=fork();assert(child>=0);
    if(!child){
        close(ready[0]);assert(!pthread_mutex_lock(mutex));
        head=NULL;head_size=0;assert(!syscall(SYS_get_robust_list,0,&head,&head_size));
        assert(head&&head_size==sizeof(*head)&&head->list.next!=&head->list);
        long registered=syscall(SYS_set_robust_list,head,head_size);
        fprintf(stderr,"STEAMOHOS_IPC_PROBE child_pid=%d child_tid=%ld lock=0x%x robust_register=%ld errno=%d\n",
            getpid(),syscall(SYS_gettid),mutex->__data.__lock,registered,errno);
        assert(write(ready[1],"L",1)==1);for(;;)pause();
    }
    close(ready[1]);assert(read(ready[0],&marker,1)==1);close(ready[0]);
    fprintf(stderr,"STEAMOHOS_IPC_PROBE phase=robust_owner_kill\n");
    assert(!kill(child,SIGKILL));assert(waitpid(child,&status,0)==child);
    fprintf(stderr,"STEAMOHOS_IPC_PROBE phase=robust_owner_lock\n");
    fprintf(stderr,"STEAMOHOS_IPC_PROBE dead_owner_lock=0x%x\n",mutex->__data.__lock);
    struct timespec recovery_deadline;assert(!clock_gettime(CLOCK_REALTIME,&recovery_deadline));
    recovery_deadline.tv_sec+=1;
    int recovered=pthread_mutex_timedlock(mutex,&recovery_deadline);
    fprintf(stderr,"STEAMOHOS_IPC_PROBE owner_recovery=%d\n",recovered);
    assert(recovered==EOWNERDEAD);
    assert(!pthread_mutex_consistent(mutex));assert(!pthread_mutex_unlock(mutex));
    assert(!pthread_mutex_destroy(mutex));assert(!shm_unlink(name));
    assert(shm_open(name,O_RDWR,0)==-1&&errno==ENOENT);
    assert(!munmap(mutex,4096));assert(!close(fd));
    puts("PASS POSIX named shm, current-thread robust list, real kernel owner death after SIGKILL");
    puts("PASS SysV key/exclusive lookup; atomic semaphore operations, SEM_UNDO after SIGKILL, timeout, removal; cross-process shared memory");
}
