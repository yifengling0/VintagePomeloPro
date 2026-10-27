// Steam depot symlinks on OHOS document storage. Included by filesystem_compat.c.
// Only configured Steam depot packages/staging are eligible. Userdata, Wine
// prefixes and unrelated libraries stay literal. Records move with depot files.
static const char DepotLinkMagic[]="STEAMOHOS_DEPOT_SYMLINK_V1\n";
// Opt-in, bounded measurements. No filenames/account data are logged.
static pthread_once_t depot_profile_once=PTHREAD_ONCE_INIT;
static int depot_profile_enabled;
static void DepotProfileSetup(void) {
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT");char path[PATH_MAX];struct stat st;
    if(ipc&&snprintf(path,sizeof(path),"%s/depot-profile.enabled",ipc)>0&&next_lstat&&
       !next_lstat(path,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0007))depot_profile_enabled=1;
}
static int DepotProfileImage(struct dl_phdr_info *image,size_t size,void *context) {
    (void)size;unsigned *count=context;if((*count)++>=128)return 1;
    const char *name=image->dlpi_name,*slash=strrchr(name,'/');if(slash)name=slash+1;
    fprintf(stderr,"STEAMOHOS_DEPOT_IMAGE pid=%d base=%p name=%.96s\n",getpid(),(void*)image->dlpi_addr,*name?name:"<main>");return 0;
}
static uint64_t DepotProfileStart(const char *path) {
    if(!path||!strstr(path,"/steamapps/"))return 0;
    int saved=errno;pthread_once(&depot_profile_once,DepotProfileSetup);
    if(!depot_profile_enabled){errno=saved;return 0;}
    static unsigned images;if(!__atomic_exchange_n(&images,1,__ATOMIC_RELAXED)){unsigned count=0;dl_iterate_phdr(DepotProfileImage,&count);}
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);errno=saved;
    return (uint64_t)ts.tv_sec*1000000+ts.tv_nsec/1000;
}
static void DepotProfileEnd(const char *operation,uint64_t started) {
    if(!started)return;
    int saved=errno;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t elapsed=(uint64_t)ts.tv_sec*1000000+ts.tv_nsec/1000-started;
    struct Metric {const char *name;uint64_t count,us,maximum;};
    // Per-thread counters need no inherited lock when Steam forks helpers.
    static _Thread_local struct Metric metrics[64];
    for(unsigned i=0;i<64;++i)if(!metrics[i].name||!strcmp(metrics[i].name,operation)) {
        struct Metric *m=&metrics[i];m->name=operation;++m->count;m->us+=elapsed;
        if(elapsed>m->maximum)m->maximum=elapsed;
        if(m->count<=2||(m->count<=4096&&m->count%256==0)||(m->count>4096&&!(m->count&(m->count-1))))
            fprintf(stderr,"STEAMOHOS_DEPOT_PROFILE pid=%d tid=%d op=%s count=%llu total_us=%llu max_us=%llu\n",getpid(),gettid(),operation,(unsigned long long)m->count,(unsigned long long)m->us,(unsigned long long)m->maximum);
        break;
    }
    errno=saved;
}
static int DepotScope(const char *path,char root[PATH_MAX]) {
    if(!path||path[0]!='/'||!strstr(path,"/steamapps/"))return 0;
    const char *libraries[]={getenv("STEAMOHOS_LIBRARY_ROOT"),getenv("STEAMOHOS_STEAM_ROOT")};
    const char *areas[]={"common","downloading","temp"};
    for(unsigned i=0;i<2;++i) {
        if(!libraries[i]||libraries[i][0]!='/')continue;
        for(unsigned kind=0;kind<3;++kind) {
            int n=snprintf(root,PATH_MAX,"%s/steamapps/%s/",libraries[i],areas[kind]);
            if(n<=0||n>=PATH_MAX||strncmp(path,root,(size_t)n))continue;
            const char *name=path+n,*end=strchr(name,'/');
            size_t length=end?(size_t)(end-name):strlen(name);
            if(!length||length>NAME_MAX||(length==1&&name[0]=='.')||
               (length==2&&name[0]=='.'&&name[1]=='.'))continue;
            if(kind) {
                // Steam staging trees use a bounded decimal AppID. Never
                // treat compatdata, userdata, depotcache or library siblings
                // as a downloaded package.
                unsigned long long id=0;int valid=length<=10&&name[0]!='0';
                for(size_t j=0;j<length&&valid;++j) {
                    valid=name[j]>='0'&&name[j]<='9';
                    if(valid)id=id*10+(unsigned)(name[j]-'0');
                }
                if(!valid||id>4294967295ULL)continue;
            }
            if((size_t)n+length>=PATH_MAX)continue;
            memcpy(root+n,name,length);root[n+length]=0;return 1;
        }
    }
    return 0;
}
static int DepotGuard(int operation) {
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT");char path[PATH_MAX];
    if(!ipc||ipc[0]!='/'){errno=EACCES;return -1;}
    int n=snprintf(path,sizeof(path),"%s/depot-links.guard",ipc);
    if(n<=0||n>=(int)sizeof(path)){errno=ENAMETOOLONG;return -1;}
    int fd=next_open(path,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)) {
        close(fd);errno=EACCES;return -1;
    }
    int result;do{result=flock(fd,operation);}while(result&&errno==EINTR);
    if(result){int error=errno;close(fd);errno=error;return -1;}
    return fd;
}
static int DepotRawTargetAtKnown(int directory,const char *path,char target[PATH_MAX],const struct stat *physical) {
    int saved=errno;struct stat st;
    // A creator may still hold the guard while this file is empty/partial.
    // Do not classify small files before waiting for its completed write.
    if(!S_ISREG(physical->st_mode)||physical->st_size<0||
       physical->st_size>=(off_t)(sizeof(DepotLinkMagic)-1+PATH_MAX)){errno=saved;return -1;}
    int guard=DepotGuard(LOCK_SH);if(guard<0)return -1;
    int fd=directory==AT_FDCWD?next_open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW):
        next_openat(directory,path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){close(guard);errno=saved;return -1;}
    char bytes[sizeof(DepotLinkMagic)-1+PATH_MAX];size_t used=0;
    while(used<sizeof(bytes)) {
        ssize_t n=read(fd,bytes+used,sizeof(bytes)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        used+=(size_t)n;
    }
    int valid=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size==(off_t)used;
    close(fd);close(guard);errno=saved;
    size_t magic=sizeof(DepotLinkMagic)-1;
    if(!valid||used<=magic||used>=sizeof(bytes)||memcmp(bytes,DepotLinkMagic,magic)||
       memchr(bytes+magic,0,used-magic))return -1;
    memcpy(target,bytes+magic,used-magic);target[used-magic]=0;return (int)(used-magic);
}
static int DepotRawTargetKnown(const char *path,char target[PATH_MAX],const struct stat *physical) {
    return DepotRawTargetAtKnown(AT_FDCWD,path,target,physical);
}
static int DepotRawTarget(const char *path,char target[PATH_MAX]) {
    int saved=errno;struct stat physical;
    if(next_lstat(path,&physical)){errno=saved;return -1;}
    return DepotRawTargetKnown(path,target,&physical);
}
// Cache only whether an OPEN directory stream belongs to a depot. Entry data
// and link targets are always re-read. A single atomic word per slot avoids
// inherited mutexes when Steam forks; closedir retires it before pointer reuse.
// Full table/unwrapped libc streams use the original uncached lookup.
static uintptr_t depot_directories[512];
static size_t DepotDirectorySlot(DIR *directory) {
    return ((uintptr_t)directory>>4)%512;
}
static int DepotPhysicalDirectoryScope(DIR *directory) {
    if(!next_readlink)return 0;
    char descriptor[64],parent[PATH_MAX],root[PATH_MAX];
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",dirfd(directory));
    ssize_t n=next_readlink(descriptor,parent,sizeof(parent)-1);if(n<0)return 0;parent[n]=0;
    return DepotScope(parent,root);
}
static void DepotRememberDirectory(DIR *directory) {
    uintptr_t key=(uintptr_t)directory;if(key&3)return;
    uintptr_t value=key|(DepotPhysicalDirectoryScope(directory)?2:1);
    size_t first=DepotDirectorySlot(directory);
    for(size_t i=0;i<512;++i) {
        uintptr_t *slot=&depot_directories[(first+i)%512];
        uintptr_t old=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
        if(old&&(old&~(uintptr_t)3)==key){__atomic_store_n(slot,value,__ATOMIC_RELEASE);return;}
    }
    for(size_t i=0;i<512;++i) {
        uintptr_t *slot=&depot_directories[(first+i)%512],old=0;
        if(__atomic_compare_exchange_n(slot,&old,value,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))return;
    }
}
static int DepotDirectoryScope(DIR *directory) {
    uintptr_t key=(uintptr_t)directory;size_t first=DepotDirectorySlot(directory);
    for(size_t i=0;i<512;++i) {
        uintptr_t value=__atomic_load_n(&depot_directories[(first+i)%512],__ATOMIC_ACQUIRE);
        if(value&&(value&~(uintptr_t)3)==key)return (value&3)==2;
    }
    return DepotPhysicalDirectoryScope(directory);
}
static void DepotForgetDirectory(DIR *directory) {
    uintptr_t key=(uintptr_t)directory;size_t first=DepotDirectorySlot(directory);
    for(size_t i=0;i<512;++i) {
        uintptr_t *slot=&depot_directories[(first+i)%512];
        uintptr_t value=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
        if(value&&(value&~(uintptr_t)3)==key) {
            __atomic_store_n(slot,0,__ATOMIC_RELEASE);return;
        }
    }
}
// Walk components before collapsing '..': link/../file is relative to the
// target's parent. Ordinary kernel paths never enter this resolver. Follow at
// most 40 records and never let a record grant access outside its package.
static const char *DepotResolveImpl(const char *path,char resolved[PATH_MAX],int follow_leaf) {
    if(!path||!*path||!next_lstat||!next_open)return path;
    char absolute[PATH_MAX],root[PATH_MAX];
    if(!AbsolutePath(path,absolute)||!DepotScope(absolute,root))return path;
    char raw[PATH_MAX];
    if(path[0]=='/') {
        if(strlen(path)>=sizeof(raw)){errno=ENAMETOOLONG;return NULL;}
        strcpy(raw,path);
    } else {
        if(!getcwd(raw,sizeof(raw)))return NULL;
        size_t n=strlen(raw),length=strlen(path);
        if(n+length+2>sizeof(raw)){errno=ENAMETOOLONG;return NULL;}
        raw[n++]='/';memcpy(raw+n,path,length+1);
    }
    // The ordinary library prefix has no virtual links. Keep uncollapsed
    // package-relative components whenever the caller provided that spelling.
    const char *source=Below(raw,root)?raw:absolute;
    char pending[PATH_MAX];strcpy(pending,source+strlen(root));strcpy(resolved,root);
    size_t boundary=strlen(root);unsigned hops=0;
    while(*pending) {
        char *part=pending;while(*part=='/')++part;
        if(!*part)break;
        char *end=part;while(*end&&*end!='/')++end;
        size_t length=(size_t)(end-part);char rest[PATH_MAX];strcpy(rest,end);
        if(length==1&&part[0]=='.'){strcpy(pending,rest);continue;}
        if(length==2&&part[0]=='.'&&part[1]=='.') {
            size_t used=strlen(resolved);
            if(used<=boundary){errno=EACCES;return NULL;}
            char *slash=strrchr(resolved,'/');*slash=0;strcpy(pending,rest);continue;
        }
        size_t used=strlen(resolved);
        if(used+length+2>PATH_MAX){errno=ENAMETOOLONG;return NULL;}
        resolved[used++]='/';memcpy(resolved+used,part,length);resolved[used+length]=0;
        // One metadata lookup per component. In particular, a real directory
        // cannot contain a record: do not lstat it twice and then stat it again.
        struct stat physical;int inspected=next_lstat(resolved,&physical);
        char target[PATH_MAX];
        if(!inspected&&(follow_leaf||*rest)&&DepotRawTargetKnown(resolved,target,&physical)>=0) {
            if(++hops>40){errno=ELOOP;return NULL;}
            char next[PATH_MAX];int n;
            if(target[0]=='/') {
                if(!Below(target,root)){errno=EACCES;return NULL;}
                n=snprintf(next,sizeof(next),"%s%s",target+boundary,rest);strcpy(resolved,root);
            } else {
                n=snprintf(next,sizeof(next),"%s%s",target,rest);resolved[used-1]=0;
            }
            if(n<0||n>=(int)sizeof(next)){errno=ENAMETOOLONG;return NULL;}
            strcpy(pending,next);continue;
        }
        if(!inspected&&S_ISLNK(physical.st_mode)) {
            // Supported kernel links keep kernel traversal (including '..'
            // and absolute targets). OHOS-created records never enter here.
            if(!hops)return path;
            size_t remaining=strlen(rest),used=strlen(resolved);
            if(used+remaining>=PATH_MAX){errno=ENAMETOOLONG;return NULL;}
            memcpy(resolved+used,rest,remaining+1);return resolved;
        }
        if(*rest) {
            if(inspected)return NULL;
            if(!S_ISDIR(physical.st_mode)){errno=ENOTDIR;return NULL;}
        }
        strcpy(pending,rest);
    }
    return resolved;
}
static const char *DepotResolve(const char *path,char resolved[PATH_MAX],int follow_leaf) {
    uint64_t started=DepotProfileStart(path);
    const char *result=DepotResolveImpl(path,resolved,follow_leaf);
    DepotProfileEnd("resolve",started);return result;
}
static int DepotLinkTarget(const char *path,char target[PATH_MAX]) {
    char resolved[PATH_MAX],root[PATH_MAX];
    const char *value=DepotResolve(path,resolved,0);
    if(!value||!DepotScope(value,root)||!strcmp(value,root))return -1;
    return DepotRawTarget(value,target);
}
static int DepotCreateLink(const char *target,const char *path) {
    size_t length=strnlen(target,PATH_MAX);
    if(!length||length>=PATH_MAX){errno=length?ENAMETOOLONG:ENOENT;return -1;}
    int guard=DepotGuard(LOCK_EX);if(guard<0)return -1;
    int fd=next_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){int error=errno;close(guard);errno=error;return -1;}
    char bytes[sizeof(DepotLinkMagic)-1+PATH_MAX];size_t magic=sizeof(DepotLinkMagic)-1;
    memcpy(bytes,DepotLinkMagic,magic);memcpy(bytes+magic,target,length);
    size_t used=0,total=magic+length;int error=0;
    while(used<total) {
        ssize_t n=write(fd,bytes+used,total-used);if(n<0&&errno==EINTR)continue;
        if(n<=0){error=n<0?errno:EIO;break;}used+=(size_t)n;
    }
    if(close(fd)&&!error)error=errno;
    if(error)next_unlink(path);
    close(guard);
    if(error){errno=error;return -1;}
    static unsigned reports;
    if(__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)<4)
        fputs("STEAMOHOS_DEPOT_SYMLINK emulated=1 scoped_depot=1\n",stderr);
    return 0;
}
