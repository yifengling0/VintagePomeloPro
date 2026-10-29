// Read-only openat2 support for native FEX's selected guest runtime.
// Walk held directory descriptors; never replace RESOLVE_IN_ROOT with a
// pathname-only open or follow an absolute guest link into the host root.
#pragma once
struct SteamOhosOpenHow { uint64_t flags,mode,resolve; };
static int FexRootRecord(int descriptor,const struct stat *status,char target[PATH_MAX]) {
    if(!S_ISREG(status->st_mode)||status->st_size<(off_t)sizeof(DepotLinkMagic)||
       status->st_size>(off_t)(sizeof(DepotLinkMagic)-1+PATH_MAX-1))return 0;
    char path[64],bytes[sizeof(DepotLinkMagic)+PATH_MAX];
    snprintf(path,sizeof(path),"/proc/self/fd/%d",descriptor);
    int readable=next_open(path,O_RDONLY|O_CLOEXEC);if(readable<0)return -1;
    struct stat actual;int valid=!fstat(readable,&actual)&&actual.st_dev==status->st_dev&&actual.st_ino==status->st_ino;
    ssize_t count=valid?pread(readable,bytes,sizeof(bytes),0):-1;
    int error=errno;close(readable);if(count<0){errno=error;return -1;}
    const size_t magic=sizeof(DepotLinkMagic)-1;
    if(count<(ssize_t)magic||memcmp(bytes,DepotLinkMagic,magic))return 0;
    size_t length=(size_t)count-magic;
    if(!length||length>=PATH_MAX||memchr(bytes+magic,0,length)){errno=EINVAL;return -1;}
    memcpy(target,bytes+magic,length);target[length]=0;return 1;
}
static int FexOpenHeld(int descriptor,int flags,int require_directory) {
    struct stat status;
    if(fstat(descriptor,&status))return -1;
    if((require_directory||(flags&O_DIRECTORY))&&!S_ISDIR(status.st_mode)){errno=ENOTDIR;return -1;}
    // Apply the same paired-interpreter overlay as open/stat/realpath after
    // the held-dirfd walk has resolved guest links and checked NOFOLLOW.
    // No arbitrary pathname replaces RESOLVE_IN_ROOT; only this exact loader
    // capability may intentionally select a file in the signed runtime.
    if(S_ISREG(status.st_mode)) {
        char proc[64],path[PATH_MAX],mapped[PATH_MAX];
        snprintf(proc,sizeof(proc),"/proc/self/fd/%d",descriptor);
        ssize_t count=next_readlink(proc,path,sizeof(path)-1);
        if(count>0&&count<(ssize_t)sizeof(path)-1) {
            path[count]=0;const char *paired=SteamOhosGuestInterpreter(path,mapped);
            if(paired!=path)return next_open(paired,flags,0);
        }
    }
    if(flags&O_PATH) {
        int result=fcntl(descriptor,F_DUPFD_CLOEXEC,0);
        if(result>=0&&!(flags&O_CLOEXEC)&&fcntl(result,F_SETFD,0)) {
            int error=errno;close(result);errno=error;return -1;
        }
        return result;
    }
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",descriptor);
    // This internal proc magic link refers to the held object. User supplied
    // symlinks were already handled, including O_NOFOLLOW, by the descriptor walk.
    return next_open(path,flags&~O_NOFOLLOW,0);
}
__attribute__((visibility("hidden"))) long SteamOhosFexOpenAt2(int directory,const char *path,const void *opaque,size_t size) {
    EnsureFilesystemSymbols();
    if(!opaque||!path){errno=EFAULT;return -1;}
    if(size<sizeof(struct SteamOhosOpenHow)){errno=EINVAL;return -1;}
    if(size>4096){errno=E2BIG;return -1;}
    const unsigned char *extra=opaque;
    for(size_t i=sizeof(struct SteamOhosOpenHow);i<size;++i)if(extra[i]){errno=E2BIG;return -1;}
    struct SteamOhosOpenHow how;memcpy(&how,opaque,sizeof(how));
    const uint64_t allowed=O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK|O_NOCTTY|O_NOATIME|O_LARGEFILE|O_PATH;
    if(how.mode||how.flags&~allowed||(how.resolve!=0&&how.resolve!=0x10)){errno=ENOSYS;return -1;}
    if(!how.resolve)return openat(directory,path,(int)how.flags,0);
    const char *root=getenv("STEAMOHOS_NATIVE_FEX_ROOTFS");
    if(!root||root[0]!='/'||directory<0){errno=ENOSYS;return -1;}
    int anchor=next_open(root,O_PATH|O_DIRECTORY|O_CLOEXEC);
    if(anchor<0)return -1;
    struct stat expected,actual;
    int valid=!fstat(anchor,&expected)&&!fstat(directory,&actual)&&S_ISDIR(actual.st_mode)&&
        actual.st_dev==expected.st_dev&&actual.st_ino==expected.st_ino;
    close(anchor);if(!valid){errno=ENOSYS;return -1;}
    size_t length=strnlen(path,PATH_MAX);
    if(!length){errno=ENOENT;return -1;}
    if(length>=PATH_MAX){errno=ENAMETOOLONG;return -1;}
    int directories[512],depth=0,result=-1,entry=-1;
    directories[0]=fcntl(directory,F_DUPFD_CLOEXEC,0);if(directories[0]<0)return -1;
    char pending[PATH_MAX],target[PATH_MAX];memcpy(pending,path,length+1);
    unsigned links=0,steps=0;
    int trailing=pending[length-1]=='/';
    while(*pending) {
        if(++steps>8192){errno=ELOOP;goto done;}
        if(pending[0]=='/') {
            while(depth>0)close(directories[depth--]);
            size_t slashes=strspn(pending,"/");memmove(pending,pending+slashes,strlen(pending+slashes)+1);
            if(!*pending)break;
        }
        char name[NAME_MAX+1];size_t component=strcspn(pending,"/");
        if(component>NAME_MAX){errno=ENAMETOOLONG;goto done;}
        memcpy(name,pending,component);name[component]=0;
        size_t consumed=component;while(pending[consumed]=='/')++consumed;
        memmove(pending,pending+consumed,strlen(pending+consumed)+1);
        if(!strcmp(name,"."))continue;
        if(!strcmp(name,"..")){if(depth>0)close(directories[depth--]);continue;}
        entry=next_openat(directories[depth],name,O_PATH|O_NOFOLLOW|O_CLOEXEC,0);
        if(entry<0)goto done;
        struct stat status;if(fstat(entry,&status))goto done;
        int link=0,emulated=0;
        if(S_ISLNK(status.st_mode)) {
            ssize_t count=next_readlinkat(entry,"",target,sizeof(target)-1);
            if(count<0)goto done;
            if(!count||count>=(ssize_t)sizeof(target)-1){errno=ENAMETOOLONG;goto done;}
            target[count]=0;link=1;
        } else {
            emulated=FexRootRecord(entry,&status,target);
            if(emulated<0)goto done;
            link=emulated;
        }
        if(link) {
            if(!*pending&&!trailing&&(how.flags&O_NOFOLLOW)) {
                if(!(how.flags&O_PATH)){errno=ELOOP;goto done;}
                if(emulated){errno=ENOSYS;goto done;}
                result=FexOpenHeld(entry,(int)how.flags,0);goto done;
            }
            if(++links>40){errno=ELOOP;goto done;}
            size_t n=strlen(target),remaining=strlen(pending);
            if(n+remaining+2>sizeof(pending)){errno=ENAMETOOLONG;goto done;}
            if(!remaining&&target[n-1]=='/')trailing=1;
            char combined[PATH_MAX];memcpy(combined,target,n);
            if(remaining){combined[n++]='/';memcpy(combined+n,pending,remaining+1);}else combined[n]=0;
            strcpy(pending,combined);close(entry);entry=-1;continue;
        }
        if(*pending) {
            if(!S_ISDIR(status.st_mode)){errno=ENOTDIR;goto done;}
            if(depth+1>=512){errno=ENAMETOOLONG;goto done;}
            directories[++depth]=entry;entry=-1;
        } else {
            result=FexOpenHeld(entry,(int)how.flags,trailing);goto done;
        }
    }
    result=FexOpenHeld(directories[depth],(int)how.flags,trailing);
done:
    {int error=errno;if(entry>=0)close(entry);while(depth>=0)close(directories[depth--]);errno=error;}
    return result;
}
