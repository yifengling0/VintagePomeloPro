// App-private GNU SysV IPC implemented with ordinary shared files and kernel file locks.
// No system policy changes. Limits are explicit; unsupported commands fail with EINVAL.
#define _GNU_SOURCE 1
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#define MAGIC 0x53564932u
#define RECORDS 256
#define NSEMS 64
#define OWNERS 64
#define SEM_KIND 1
#define SHM_KIND 2
struct Record { int kind, key, id; };
struct Registry { unsigned magic; int next; struct Record records[RECORDS]; };
struct Undo { pid_t pid; uint64_t birth; int delta[NSEMS]; };
struct SemState { unsigned short values[NSEMS]; pid_t pids[NSEMS]; time_t otime; struct Undo undo[OWNERS]; };
struct Sem { unsigned magic, removed, active, count, mode; uid_t uid; gid_t gid; time_t ctime; struct SemState state[2]; };
struct Attachment { pid_t pid; uint64_t birth; unsigned count; };
struct Shm { unsigned magic, removed, mode; uid_t uid; gid_t gid; size_t size; time_t ctime, atime, dtime; pid_t cpid, lpid; struct Attachment users[OWNERS]; };
union SteamSemun { int val; struct semid_ds *buf; unsigned short *array; };
static int Fail(int error) { errno = error; return -1; }
static int Root(void) {
    const char *path = getenv("STEAMOHOS_IPC_ROOT");
    if (!path || path[0] != '/') return Fail(ENOSYS);
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0077))) {
        close(fd); return Fail(EACCES);
    }
    return fd;
}
static int Lock(int fd) {
    int result; do { result = flock(fd, LOCK_EX); } while (result && errno == EINTR);
    return result;
}
static void Name(char *name, size_t length, int kind, int id) { snprintf(name, length, "%c-%d", kind == SEM_KIND ? 's' : 'm', id); }
static uint64_t Birth(pid_t pid) {
    char path[64], data[1024]; snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t length = read(fd, data, sizeof(data)-1); close(fd); if (length <= 0) return 0;
    data[length] = 0; char *cursor = strrchr(data, ')'); if (!cursor) return 0;
    ++cursor;
    for (int field = 3; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        if (field == 22) return strtoull(cursor, NULL, 10);
        cursor = strchr(cursor, ' '); if (!cursor) return 0;
    }
    return 0;
}
static int Alive(pid_t pid, uint64_t birth) {
    if (pid == getpid()) return !birth || !Birth(pid) || Birth(pid) == birth;
    if (kill(pid, 0) && errno == ESRCH) return 0;
    uint64_t current = birth ? Birth(pid) : 0;
    return !birth || !current || current == birth;
}
static int RegistryOpen(int root, struct Registry **registry) {
    int fd = openat(root, "registry-v2", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (Lock(fd) || ftruncate(fd, sizeof(**registry))) { int e=errno; close(fd); return Fail(e); }
    *registry = mmap(NULL, sizeof(**registry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*registry == MAP_FAILED) { int e=errno; close(fd); return Fail(e); }
    if ((*registry)->magic != MAGIC) { memset(*registry, 0, sizeof(**registry)); (*registry)->next=1; (*registry)->magic=MAGIC; }
    return fd;
}
static void RegistryClose(int fd, struct Registry *registry) { munmap(registry, sizeof(*registry)); flock(fd,LOCK_UN); close(fd); }
static int Get(int kind, key_t key, size_t amount, int flags) {
    if ((kind == SEM_KIND && amount > NSEMS) || (kind == SHM_KIND && amount > 256u*1024u*1024u)) return Fail(EINVAL);
    int root = Root(); if (root < 0) return -1;
    struct Registry *registry; int fd = RegistryOpen(root, &registry);
    if (fd < 0) { close(root); return -1; }
    int free_slot = -1, id = -1, error = ENOENT;
    for (int i=0; i<RECORDS; ++i) {
        struct Record *record=&registry->records[i];
        if (!record->kind && free_slot<0) free_slot=i;
        if (key != IPC_PRIVATE && record->kind == kind && record->key == key) {
            if ((flags & (IPC_CREAT|IPC_EXCL)) == (IPC_CREAT|IPC_EXCL)) { error=EEXIST; break; }
            char name[40]; Name(name,sizeof(name),kind,record->id);
            int existing=openat(root,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
            if (existing<0) { error=errno; break; }
            size_t size=0;
            if (kind == SEM_KIND) { struct Sem header; if (pread(existing,&header,sizeof(header),0)==sizeof(header)) size=header.count; }
            else { struct Shm header; if (pread(existing,&header,sizeof(header),0)==sizeof(header)) size=header.size; }
            close(existing);
            if (amount > size) { error=EINVAL; break; }
            id=record->id; break;
        }
    }
    if (id<0 && error==ENOENT && (key==IPC_PRIVATE || (flags&IPC_CREAT))) {
        if (!amount) error=EINVAL;
        else if (free_slot<0 || registry->next==INT32_MAX) error=ENOSPC;
        else {
            int candidate=registry->next++; char name[40]; Name(name,sizeof(name),kind,candidate);
            int file=openat(root,name,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if (file<0) error=errno;
            else {
                size_t bytes=kind==SEM_KIND?sizeof(struct Sem):(size_t)sysconf(_SC_PAGESIZE)+amount;
                if (ftruncate(file,bytes)) error=errno;
                else if (kind==SEM_KIND) {
                    struct Sem *sem=mmap(NULL,sizeof(*sem),PROT_READ|PROT_WRITE,MAP_SHARED,file,0);
                    if (sem==MAP_FAILED) error=errno;
                    else { memset(sem,0,sizeof(*sem)); sem->magic=MAGIC; sem->count=amount; sem->mode=flags&0777;
                        sem->uid=geteuid(); sem->gid=getegid(); sem->ctime=time(NULL); munmap(sem,sizeof(*sem)); id=candidate; }
                } else {
                    struct Shm header={.magic=MAGIC,.mode=flags&0777,.uid=geteuid(),.gid=getegid(),.size=amount,.ctime=time(NULL),.cpid=getpid()};
                    if (pwrite(file,&header,sizeof(header),0)!=sizeof(header)) error=EIO; else id=candidate;
                }
                close(file);
                if (id>=0) registry->records[free_slot]=(struct Record){kind,key,id}; else unlinkat(root,name,0);
            }
        }
    }
    RegistryClose(fd,registry); close(root); return id>=0?id:Fail(error);
}
static void RemoveKey(int kind, int id) {
    int root=Root(); if(root<0) return;
    struct Registry *registry; int fd=RegistryOpen(root,&registry);
    if(fd>=0) { for(int i=0;i<RECORDS;++i) if(registry->records[i].kind==kind && registry->records[i].id==id) registry->records[i].kind=0; RegistryClose(fd,registry); }
    close(root);
}
static void UnlinkGroup(int kind,int id) {
    int root=Root();if(root<0)return;char name[40];Name(name,sizeof(name),kind,id);unlinkat(root,name,0);close(root);
}
static int GroupOpen(int kind,int id,size_t bytes,void **memory) {
    if(id<=0) return Fail(EINVAL);
    int root=Root(); if(root<0)return -1;
    char name[40];Name(name,sizeof(name),kind,id);
    int fd=openat(root,name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);close(root);
    if(fd<0) { if(errno==ENOENT)errno=EINVAL;return -1; }
    if(Lock(fd)) {int e=errno;close(fd);return Fail(e);}
    *memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(*memory==MAP_FAILED){int e=errno;close(fd);return Fail(e);}
    if(*(unsigned *)*memory!=MAGIC){munmap(*memory,bytes);close(fd);return Fail(EINVAL);}
    return fd;
}
static void GroupClose(int fd,void *memory,size_t bytes){munmap(memory,bytes);flock(fd,LOCK_UN);close(fd);}
static struct SemState *Prepare(struct Sem *sem) {
    struct SemState *next=&sem->state[1-sem->active]; *next=sem->state[sem->active];
    for(unsigned i=0;i<OWNERS;++i) if(next->undo[i].pid && !Alive(next->undo[i].pid,next->undo[i].birth)) {
        for(unsigned n=0;n<sem->count;++n){int value=(int)next->values[n]+next->undo[i].delta[n];next->values[n]=value<0?0:value>32767?32767:value;}
        memset(&next->undo[i],0,sizeof(next->undo[i]));
    }
    return next;
}
static void Commit(struct Sem *sem) { __atomic_store_n(&sem->active,1-sem->active,__ATOMIC_RELEASE); }
int semget(key_t key,int count,int flags){if(count<0)return Fail(EINVAL);return Get(SEM_KIND,key,count,flags);}
int semctl(int id,int number,int command,...) {
    union SteamSemun argument={0}; command &= ~0x100;
    if(command==SETVAL||command==SETALL||command==GETALL||command==IPC_STAT||command==IPC_SET){va_list args;va_start(args,command);argument=va_arg(args,union SteamSemun);va_end(args);}
    struct Sem *sem;int fd=GroupOpen(SEM_KIND,id,sizeof(*sem),(void**)&sem);if(fd<0)return -1;
    int result=-1,error=EINVAL;struct SemState *state=Prepare(sem);
    if(sem->removed)error=EIDRM;
    else if(command==IPC_RMID){sem->removed=1;RemoveKey(SEM_KIND,id);UnlinkGroup(SEM_KIND,id);result=0;}
    else if(command==IPC_STAT && argument.buf){memset(argument.buf,0,sizeof(*argument.buf));argument.buf->sem_perm.uid=argument.buf->sem_perm.cuid=sem->uid;argument.buf->sem_perm.gid=argument.buf->sem_perm.cgid=sem->gid;argument.buf->sem_perm.mode=sem->mode;argument.buf->sem_nsems=sem->count;argument.buf->sem_ctime=sem->ctime;argument.buf->sem_otime=state->otime;result=0;}
    else if(command==IPC_SET && argument.buf){sem->mode=argument.buf->sem_perm.mode&0777;sem->ctime=time(NULL);result=0;}
    else if(command==GETALL && argument.array){memcpy(argument.array,state->values,sem->count*sizeof(unsigned short));result=0;}
    else if(command==SETALL && argument.array){result=0;for(unsigned i=0;i<sem->count;++i)if(argument.array[i]>32767){result=-1;error=ERANGE;break;}if(!result){memcpy(state->values,argument.array,sem->count*sizeof(unsigned short));memset(state->undo,0,sizeof(state->undo));sem->ctime=time(NULL);}}
    else if(number>=0 && (unsigned)number<sem->count){
        if(command==GETVAL)result=state->values[number];
        else if(command==GETPID)result=state->pids[number];
        else if(command==SETVAL){if(argument.val<0||argument.val>32767)error=ERANGE;else{state->values[number]=argument.val;state->pids[number]=getpid();for(unsigned i=0;i<OWNERS;++i)state->undo[i].delta[number]=0;sem->ctime=time(NULL);result=0;}}
    }
    Commit(sem);GroupClose(fd,sem,sizeof(*sem));return result<0?Fail(error):result;
}
static int64_t Nanoseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
int semtimedop(int id,struct sembuf *operations,size_t count,const struct timespec *timeout){
    if(!operations)return Fail(EFAULT);
    if(!count||count>256)return Fail(E2BIG);
    if(timeout&&(timeout->tv_sec<0||timeout->tv_nsec<0||timeout->tv_nsec>=1000000000))return Fail(EINVAL);
    int64_t deadline=timeout?Nanoseconds()+(int64_t)timeout->tv_sec*1000000000+timeout->tv_nsec:INT64_MAX;
    int waited=0;
    for(;;){
        struct Sem *sem;int fd=GroupOpen(SEM_KIND,id,sizeof(*sem),(void**)&sem);if(fd<0)return waited&&errno==EINVAL?Fail(EIDRM):-1;
        struct SemState *state=Prepare(sem);Commit(sem);state=Prepare(sem);
        int error=0,waiting=0;
        if(sem->removed)error=EIDRM;
        for(size_t i=0;!error&&i<count;++i){struct sembuf op=operations[i];
            if(op.sem_num>=sem->count){error=EFBIG;break;}
            if((op.sem_op?0200:0400)&~sem->mode){error=EACCES;break;}
            int value=(int)state->values[op.sem_num]+op.sem_op;
            if((op.sem_op==0&&value!=0)||value<0){if(op.sem_flg&IPC_NOWAIT)error=EAGAIN;else waiting=1;break;}
            if(value>32767){error=ERANGE;break;}
            if(op.sem_flg&SEM_UNDO){int slot=-1;pid_t pid=getpid();for(int j=0;j<OWNERS;++j)if(state->undo[j].pid==pid){slot=j;break;}else if(!state->undo[j].pid&&slot<0)slot=j;
                if(slot<0){error=ENOSPC;break;}struct Undo *undo=&state->undo[slot];int adjust=undo->delta[op.sem_num]-op.sem_op;
                if(adjust<-32767||adjust>32767){error=ERANGE;break;}undo->pid=pid;undo->birth=Birth(pid);undo->delta[op.sem_num]=adjust;
            }
            state->values[op.sem_num]=value;state->pids[op.sem_num]=getpid();
        }
        if(!error&&!waiting){state->otime=time(NULL);Commit(sem);}
        GroupClose(fd,sem,sizeof(*sem));
        if(error)return Fail(error);
        if(!waiting)return 0;
        if(Nanoseconds()>=deadline)return Fail(EAGAIN);
        waited=1;
        struct timespec pause={0,10000000};if(nanosleep(&pause,NULL))return -1;
    }
}
int semop(int id,struct sembuf *operations,size_t count){return semtimedop(id,operations,count,NULL);}

int shmget(key_t key,size_t bytes,int flags){return Get(SHM_KIND,key,bytes,flags);}
struct LocalMapping {void *address;size_t bytes;int id;};
static struct LocalMapping mappings[128];
static pthread_mutex_t mapping_mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned Users(struct Shm *shm){unsigned total=0;for(int i=0;i<OWNERS;++i){if(shm->users[i].pid&&!Alive(shm->users[i].pid,shm->users[i].birth))memset(&shm->users[i],0,sizeof(shm->users[i]));total+=shm->users[i].count;}return total;}
void *shmat(int id,const void *requested,int flags){
    struct Shm *shm;int fd=GroupOpen(SHM_KIND,id,sizeof(*shm),(void**)&shm);if(fd<0)return (void*)-1;
    Users(shm);int error=0,slot=-1;pid_t pid=getpid();
    for(int i=0;i<OWNERS;++i)if(shm->users[i].pid==pid){slot=i;break;}else if(!shm->users[i].pid&&slot<0)slot=i;
    if(slot<0)error=ENOSPC;
    if(flags&~(SHM_RDONLY|SHM_RND))error=EINVAL;
    size_t page=sysconf(_SC_PAGESIZE);void *address=(void*)requested;
    if(flags&SHM_RND)address=(void*)((uintptr_t)address&~(page-1));
    if(address&&(uintptr_t)address%page)error=EINVAL;
    void *mapped=MAP_FAILED;size_t bytes=shm->size;
    if(!error)mapped=mmap(address,bytes,PROT_READ|((flags&SHM_RDONLY)?0:PROT_WRITE),MAP_SHARED|(address?MAP_FIXED_NOREPLACE:0),fd,page);
    if(mapped==MAP_FAILED&&!error)error=errno;
    if(!error){pthread_mutex_lock(&mapping_mutex);int local=-1;for(int i=0;i<128;++i)if(!mappings[i].address){local=i;break;}
        if(local<0){munmap(mapped,bytes);error=ENOMEM;}else{mappings[local]=(struct LocalMapping){mapped,bytes,id};shm->users[slot]=(struct Attachment){pid,Birth(pid),shm->users[slot].count+1};shm->atime=time(NULL);shm->lpid=pid;}
        pthread_mutex_unlock(&mapping_mutex);
    }
    GroupClose(fd,shm,sizeof(*shm));if(error){errno=error;return (void*)-1;}return mapped;
}
int shmdt(const void *address){
    pthread_mutex_lock(&mapping_mutex);int local=-1;for(int i=0;i<128;++i)if(mappings[i].address==address){local=i;break;}
    if(local<0){pthread_mutex_unlock(&mapping_mutex);return Fail(EINVAL);}
    struct LocalMapping mapping=mappings[local];mappings[local].address=NULL;pthread_mutex_unlock(&mapping_mutex);
    int result=munmap(mapping.address,mapping.bytes);struct Shm *shm;int fd=GroupOpen(SHM_KIND,mapping.id,sizeof(*shm),(void**)&shm);
    if(fd>=0){for(int i=0;i<OWNERS;++i)if(shm->users[i].pid==getpid()&&shm->users[i].count){--shm->users[i].count;break;}shm->dtime=time(NULL);shm->lpid=getpid();if(shm->removed&&!Users(shm))UnlinkGroup(SHM_KIND,mapping.id);GroupClose(fd,shm,sizeof(*shm));}
    return result;
}
int shmctl(int id,int command,struct shmid_ds *buffer){
    command&=~0x100;struct Shm *shm;int fd=GroupOpen(SHM_KIND,id,sizeof(*shm),(void**)&shm);if(fd<0)return -1;
    int result=-1,error=EINVAL;unsigned users=Users(shm);
    if(command==IPC_RMID){shm->removed=1;RemoveKey(SHM_KIND,id);if(!users)UnlinkGroup(SHM_KIND,id);result=0;}
    else if(command==IPC_STAT&&buffer){memset(buffer,0,sizeof(*buffer));buffer->shm_perm.uid=buffer->shm_perm.cuid=shm->uid;buffer->shm_perm.gid=buffer->shm_perm.cgid=shm->gid;buffer->shm_perm.mode=shm->mode|(shm->removed?SHM_DEST:0);buffer->shm_segsz=shm->size;buffer->shm_nattch=users;buffer->shm_cpid=shm->cpid;buffer->shm_lpid=shm->lpid;buffer->shm_ctime=shm->ctime;buffer->shm_atime=shm->atime;buffer->shm_dtime=shm->dtime;result=0;}
    else if(command==IPC_SET&&buffer){shm->mode=buffer->shm_perm.mode&0777;shm->ctime=time(NULL);result=0;}
    GroupClose(fd,shm,sizeof(*shm));return result<0?Fail(error):result;
}
