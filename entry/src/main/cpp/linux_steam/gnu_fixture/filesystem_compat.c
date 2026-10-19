// GNU-only temporary namespace and native entry identity for the app sandbox.
#define _GNU_SOURCE 1
#include <sys/types.h>
#include <sys/time.h>
#include <utime.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>

static int (*next_stat)(const char *, struct stat *);
static ssize_t (*next_readlink)(const char*,char*,size_t);
static ssize_t (*next_readlinkat)(int,const char*,char*,size_t);
static void TraceAtFailure(int,const char*,int,int);
static void TraceProcAccess(const char*,const char*,int,int);
static int SingletonName(const char*);
static int SingletonTarget(const char*,char[PATH_MAX]);
static const char *FollowSingleton(const char*,char[PATH_MAX]);
static void InitializeSingletonProfile(void);
static const char *AtPath(int,const char*,char[PATH_MAX]);
static const char *SandboxAtPath(int,const char*,char[PATH_MAX],char[PATH_MAX]);
static int AbsolutePath(const char *path, char normalized[PATH_MAX]) {
    char combined[PATH_MAX];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(combined)) return 0;
        strcpy(combined,path);
    } else {
        if (!getcwd(combined,sizeof(combined))) return 0;
        size_t used=strlen(combined), length=strlen(path);
        if (used+length+2>sizeof(combined)) return 0;
        combined[used++]='/';memcpy(combined+used,path,length+1);
    }
    size_t used=1;normalized[0]='/';normalized[1]=0;
    const char *part=combined;
    while (*part) {
        while (*part=='/')++part;
        const char *end=part;while(*end&&*end!='/')++end;
        size_t length=(size_t)(end-part);
        if (!length) break;
        if (length==1&&part[0]=='.') {}
        else if (length==2&&part[0]=='.'&&part[1]=='.') {
            while(used>1&&normalized[used-1]!='/')--used;
            if(used>1)--used;
            normalized[used]=0;
        } else {
            if(used>1)normalized[used++]='/';
            memcpy(normalized+used,part,length);used+=length;normalized[used]=0;
        }
        part=end;
    }
    return 1;
}
static int Below(const char *path,const char *root) {
    size_t length=strlen(root);
    return !strncmp(path,root,length)&&(path[length]=='/'||path[length]==0);
}
static const char *CloudCompatPath(const char *,char[PATH_MAX]);
static int CloudLinkTarget(const char *,char[PATH_MAX]);
static int CloudLinkPath(const char *,char[PATH_MAX],char[PATH_MAX],const char **);
static const char *FollowCloud(const char *,char[PATH_MAX],int);
// Keep the built-in Steam library as the only visible target, with all
// steamapps data in the existing Z: game library. Activation follows the
// host's completed, conflict-checked migration; unrelated paths stay literal.
static int single_library;
static void InitializeSingleLibrary(void) {
    const char *library=getenv("STEAMOHOS_LIBRARY_ROOT");
    if(!library||library[0]!='/'||!next_stat)return;
    char marker[PATH_MAX];struct stat status;
    int n=snprintf(marker,sizeof(marker),"%s/steamapps/.hmosdeck-single-library-v2",library);
    single_library=n>0&&n<(int)sizeof(marker)&&!next_stat(marker,&status)&&S_ISREG(status.st_mode);
}
static const char *LibraryAppsPath(const char *path,char mapped[PATH_MAX]) {
    if(!single_library)return path;
    const char *library=getenv("STEAMOHOS_LIBRARY_ROOT"),*home=getenv("HOME");
    char absolute[PATH_MAX],home_alias[PATH_MAX];
    if(!library||library[0]!='/'||!AbsolutePath(path,absolute))return path;
    int n=home?snprintf(home_alias,sizeof(home_alias),"%s/.steam/root",home):0;
    const char *roots[]={getenv("STEAMOHOS_STEAM_ROOT"),getenv("STEAMOHOS_CODE_ROOT"),
                        n>0&&n<(int)sizeof(home_alias)?home_alias:NULL};
    for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);++i) {
        if(!roots[i]||roots[i][0]!='/')continue;
        char apps[PATH_MAX];n=snprintf(apps,sizeof(apps),"%s/steamapps",roots[i]);
        if(n<=0||n>=(int)sizeof(apps)||!Below(absolute,apps))continue;
        n=snprintf(mapped,PATH_MAX,"%s/steamapps%s",library,absolute+strlen(apps));
        return n>0&&n<PATH_MAX?mapped:path;
    }
    return path;
}
static int PrivateShmDirectory(const char *name, const char **leaf);
static const char *SandboxPath(const char *path, char mapped[PATH_MAX]) {
    if (!path || !*path) return path;
    // Chromium opens /dev/shm files directly rather than calling shm_open.
    // Both APIs must see the same private inodes. Restrict this alias to the
    // shared-memory directory and flat POSIX names; unrelated /dev paths and
    // traversal retain the real filesystem behavior.
    if (getenv("STEAMOHOS_IPC_ROOT") &&
        (!strcmp(path,"/dev/shm") || !strncmp(path,"/dev/shm/",9))) {
        const char *name=path[8]=='/'?path+9:"";
        if (!strchr(name,'/') && strcmp(name,".") && strcmp(name,"..")) {
            const char *unused=NULL;
            int directory=PrivateShmDirectory(*name?name:".directory-probe",&unused);
            if(directory>=0) {
                close(directory);
                int size=snprintf(mapped,PATH_MAX,"%s/posix-shm%s%s",getenv("STEAMOHOS_IPC_ROOT"),*name?"/":"",name);
                if(size>0&&size<PATH_MAX)return mapped;
            }
        }
    }
    char library_path[PATH_MAX];
    const char *library=LibraryAppsPath(path,library_path);
    if(library!=path) {
        if(CloudCompatPath(library,mapped)==library)strcpy(mapped,library);
        return mapped;
    }
    if(CloudCompatPath(path,mapped)!=path)return mapped;
    // Steam's embedded OpenSSL first tries these distribution-default paths.
    // Keep real certificate verification with packaged Mozilla roots.
    if (!strcmp(path, "/etc/ssl/certs/ca-certificates.crt") || !strcmp(path, "/etc/ssl/ca-bundle.pem")) {
        const char *bundle = getenv("SSL_CERT_FILE");
        if (bundle && bundle[0] == '/' && strlen(bundle) < PATH_MAX) {
            strcpy(mapped, bundle); return mapped;
        }
    }

    const char *data=getenv("STEAMOHOS_STEAM_ROOT"), *code=getenv("STEAMOHOS_CODE_ROOT");
    const char *home=getenv("HOME"), *ipc=getenv("STEAMOHOS_IPC_ROOT");
    if ((data&&code)||(home&&ipc)) {
        char absolute[PATH_MAX];
        if (AbsolutePath(path,absolute)) {
            // Browser profiles require atomic locks and Unix endpoints. Keep
            // this writable cache private; original client resources stay on Z:.
            char profile[PATH_MAX];
            if(data&&ipc&&snprintf(profile,sizeof(profile),"%s/config/htmlcache",data)>0&&Below(absolute,profile)) {
                int size=snprintf(mapped,PATH_MAX,"%s/cef-profile%s",ipc,absolute+strlen(profile));
                if(size>0&&size<PATH_MAX)return mapped;
            }
            if(SingletonName(absolute)) {strcpy(mapped,absolute);return mapped;}
            // NSS needs ordinary SQLite locking; keep certificates/databases
            // in the app-private filesystem rather than the Z: document store.
            char pki[PATH_MAX];
            if(home&&ipc&&snprintf(pki,sizeof(pki),"%s/.pki",home)>0&&Below(absolute,pki)) {
                int size=snprintf(mapped,PATH_MAX,"%s/pki%s",ipc,absolute+strlen(pki));
                if(size>0&&size<PATH_MAX)return mapped;
            }
            // The shared document provider cannot hold FIFOs. Keep only this
            // Unix IPC endpoint private while all client resources stay on Z:.
            char pipe_path[PATH_MAX];
            if (home&&ipc&&snprintf(pipe_path,sizeof(pipe_path),"%s/.steam/steam.pipe",home)>0 && !strcmp(absolute,pipe_path)) {
                int size=snprintf(mapped,PATH_MAX,"%s/steam.pipe",ipc);
                if(size>0&&size<PATH_MAX)return mapped;
            }
            // ~/.steam/root is normally a symlink to the install directory.
            // The document provider cannot create links; resolve this one
            // configured alias without changing unrelated filesystem paths.
            char alias[PATH_MAX];
            if(home&&data&&snprintf(alias,sizeof(alias),"%s/.steam/root",home)>0&&Below(absolute,alias)) {
                int size=snprintf(mapped,PATH_MAX,"%s%s",data,absolute+strlen(alias));
                if(size>0&&size<PATH_MAX)return mapped;
            }
            if (data&&code&&data[0]=='/'&&code[0]=='/'&&next_stat&&Below(absolute,code)) {
                int size=snprintf(mapped,PATH_MAX,"%s%s",data,absolute+strlen(code));
                struct stat status;
                if(size>0&&size<PATH_MAX&&!next_stat(mapped,&status)) return mapped;
            }
        }
    }
    if (strcmp(path, "/tmp") && strncmp(path, "/tmp/", 5)) return path;
    const char *root = getenv("STEAMOHOS_TMP_ROOT");
    if (!root || root[0] != '/' || strstr(path, "/../") || !strcmp(path + strlen(path) - 3, "/..")) return path;
    int size = snprintf(mapped, PATH_MAX, "%s%s", root, path + 4);
    if (size < 0 || size >= PATH_MAX) return path;
    return mapped;
}
#define FS(result, name, parameters, arguments, failure) \
    static result (*next_##name) parameters; \
    result name parameters { \
        if (!next_##name) { errno=ENOSYS; return failure; } \
        char mapped[PATH_MAX],followed[PATH_MAX],cloud[PATH_MAX]; path=SandboxPath(path,mapped); \
        path=FollowCloud(path,cloud,strcmp(#name,"unlink")&&strcmp(#name,"unlinkat")&&strcmp(#name,"rmdir")&&strcmp(#name,"lchown")); \
        if(strcmp(#name,"unlink")&&strcmp(#name,"unlinkat")&&strcmp(#name,"lstat")&&strcmp(#name,"lstat64")&& \
           strcmp(#name,"__lxstat")&&strcmp(#name,"__lxstat64"))path=FollowSingleton(path,followed); \
        result value=next_##name arguments; int saved=errno; \
        TraceProcAccess(#name,path,value==failure,saved); errno=saved; return value; \
    }

#define AT_FS(result,name,parameters,arguments,failure) \
    static result (*next_##name) parameters; \
    result name parameters { \
        if(!next_##name){errno=ENOSYS;return failure;} \
        char absolute[PATH_MAX],mapped[PATH_MAX],cloud[PATH_MAX]; \
        path=SandboxAtPath(fd,path,absolute,mapped); \
        path=FollowCloud(path,cloud,strcmp(#name,"unlinkat")); \
        return next_##name arguments; \
    }
FS(int, chdir, (const char *path), (path), -1)
FS(int, mknod, (const char *path, mode_t mode, dev_t device), (path,mode,device), -1)
AT_FS(int, mknodat, (int fd, const char *path, mode_t mode, dev_t device), (fd,path,mode,device), -1)
FS(int, __xmknod, (int version, const char *path, mode_t mode, dev_t *device), (version,path,mode,device), -1)
AT_FS(int, __xmknodat, (int version, int fd, const char *path, mode_t mode, dev_t *device), (version,fd,path,mode,device), -1)
FS(int, mkdir, (const char *path, mode_t mode), (path,mode), -1)
AT_FS(int, mkdirat, (int fd, const char *path, mode_t mode), (fd,path,mode), -1)
FS(int, stat, (const char *path, struct stat *buf), (path,buf), -1)
static int (*next_lstat)(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf) {
    if(!next_lstat){errno=ENOSYS;return -1;}
    char mapped[PATH_MAX],cloud[PATH_MAX];path=FollowCloud(SandboxPath(path,mapped),cloud,0);
    int result=next_lstat(path,buf);int error=errno;
    TraceProcAccess("lstat",path,result<0,error);
    if(!result){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
FS(int, stat64, (const char *path, struct stat64 *buf), (path,buf), -1)
static int (*next_lstat64)(const char *path, struct stat64 *buf);
int lstat64(const char *path, struct stat64 *buf) {
    if(!next_lstat64){errno=ENOSYS;return -1;}
    char mapped[PATH_MAX],cloud[PATH_MAX];path=FollowCloud(SandboxPath(path,mapped),cloud,0);
    int result=next_lstat64(path,buf);int error=errno;
    TraceProcAccess("lstat64",path,result<0,error);
    if(!result){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
FS(int, __xstat, (int version, const char *path, struct stat *buf), (version,path,buf), -1)
static int (*next___lxstat)(int version, const char *path, struct stat *buf);
int __lxstat(int version, const char *path, struct stat *buf) {
    if(!next___lxstat){errno=ENOSYS;return -1;}
    char mapped[PATH_MAX],cloud[PATH_MAX];path=FollowCloud(SandboxPath(path,mapped),cloud,0);
    int result=next___lxstat(version,path,buf);int error=errno;
    TraceProcAccess("__lxstat",path,result<0,error);
    if(!result){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
FS(int, __xstat64, (int version, const char *path, struct stat64 *buf), (version,path,buf), -1)
static int (*next___lxstat64)(int version, const char *path, struct stat64 *buf);
int __lxstat64(int version, const char *path, struct stat64 *buf) {
    if(!next___lxstat64){errno=ENOSYS;return -1;}
    char mapped[PATH_MAX],cloud[PATH_MAX];path=FollowCloud(SandboxPath(path,mapped),cloud,0);
    int result=next___lxstat64(version,path,buf);int error=errno;
    TraceProcAccess("__lxstat64",path,result<0,error);
    if(!result){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
static int (*next_fstatat)(int,const char*,struct stat*,int);
int fstatat(int fd,const char *path,struct stat *buf,int flags) {
    if(!next_fstatat){errno=ENOSYS;return -1;}
    char absolute[PATH_MAX],mapped[PATH_MAX],followed[PATH_MAX],cloud[PATH_MAX];
    path=SandboxAtPath(fd,path,absolute,mapped);
    path=FollowCloud(path,cloud,!(flags&AT_SYMLINK_NOFOLLOW));
    if(!(flags&AT_SYMLINK_NOFOLLOW))path=FollowSingleton(path,followed);
    int result=next_fstatat(fd,path,buf,flags);int error=errno;
    if(result)TraceAtFailure(fd,path,flags,error);
    if(!result&&(flags&AT_SYMLINK_NOFOLLOW)){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
static int (*next_fstatat64)(int,const char*,struct stat64*,int);
int fstatat64(int fd,const char *path,struct stat64 *buf,int flags) {
    if(!next_fstatat64){errno=ENOSYS;return -1;}
    char absolute[PATH_MAX],mapped[PATH_MAX],followed[PATH_MAX],cloud[PATH_MAX];
    path=SandboxAtPath(fd,path,absolute,mapped);
    path=FollowCloud(path,cloud,!(flags&AT_SYMLINK_NOFOLLOW));
    if(!(flags&AT_SYMLINK_NOFOLLOW))path=FollowSingleton(path,followed);
    int result=next_fstatat64(fd,path,buf,flags);int error=errno;
    if(result)TraceAtFailure(fd,path,flags,error);
    if(!result&&(flags&AT_SYMLINK_NOFOLLOW)){char target[PATH_MAX];int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
        if(length>=0){buf->st_mode=S_IFLNK|0777;buf->st_size=length;buf->st_blocks=0;}}
    errno=error;return result;
}
FS(int, access, (const char *path, int mode), (path,mode), -1)
AT_FS(int, faccessat, (int fd, const char *path, int mode, int flags), (fd,path,mode,flags), -1)
FS(int, chmod, (const char *path, mode_t mode), (path,mode), -1)
FS(int, utime, (const char *path, const struct utimbuf *times), (path,times), -1)
FS(int, utimes, (const char *path, const struct timeval times[2]), (path,times), -1)
FS(int, chown, (const char *path, uid_t uid, gid_t gid), (path,uid,gid), -1)
FS(int, lchown, (const char *path, uid_t uid, gid_t gid), (path,uid,gid), -1)
FS(int, unlink, (const char *path), (path), -1)
AT_FS(int, unlinkat, (int fd, const char *path, int flags), (fd,path,flags), -1)
FS(int, rmdir, (const char *path), (path), -1)
FS(int, mkfifo, (const char *path, mode_t mode), (path,mode), -1)
FS(int, statfs, (const char *path, struct statfs *buf), (path,buf), -1)
FS(int, statfs64, (const char *path, struct statfs64 *buf), (path,buf), -1)
FS(int, statvfs, (const char *path, struct statvfs *buf), (path,buf), -1)
FS(int, statvfs64, (const char *path, struct statvfs64 *buf), (path,buf), -1)
FS(FILE *, fopen, (const char *path, const char *mode), (path,mode), NULL)
FS(FILE *, fopen64, (const char *path, const char *mode), (path,mode), NULL)
FS(FILE *, freopen, (const char *path, const char *mode, FILE *stream), (path,mode,stream), NULL)
FS(FILE *, freopen64, (const char *path, const char *mode, FILE *stream), (path,mode,stream), NULL)
FS(int, scandir, (const char *path, struct dirent ***names,
   int (*filter)(const struct dirent *), int (*compare)(const struct dirent **,const struct dirent **)),
   (path,names,filter,compare), -1)
FS(int, scandir64, (const char *path, struct dirent64 ***names,
   int (*filter)(const struct dirent64 *), int (*compare)(const struct dirent64 **,const struct dirent64 **)),
   (path,names,filter,compare), -1)
AT_FS(int, scandirat, (int fd, const char *path, struct dirent ***names,
   int (*filter)(const struct dirent *), int (*compare)(const struct dirent **,const struct dirent **)),
   (fd,path,names,filter,compare), -1)
AT_FS(int, scandirat64, (int fd, const char *path, struct dirent64 ***names,
   int (*filter)(const struct dirent64 *), int (*compare)(const struct dirent64 **,const struct dirent64 **)),
   (fd,path,names,filter,compare), -1)
FS(DIR *, opendir, (const char *path), (path), NULL)

#define FORTIFIED_OPEN(name) \
    static int (*next_##name)(const char*,int); \
    int name(const char *path,int flags) { \
        if(!next_##name){errno=ENOSYS;return -1;} \
        char mapped[PATH_MAX],cloud[PATH_MAX],target[PATH_MAX],singleton[PATH_MAX]; \
        path=SandboxPath(path,mapped); \
        if((flags&O_NOFOLLOW)&&CloudLinkTarget(path,target)>=0){errno=ELOOP;return -1;} \
        path=FollowCloud(path,cloud,1);path=FollowSingleton(path,singleton); \
        return next_##name(path,flags); \
    }
FORTIFIED_OPEN(__open_2)
FORTIFIED_OPEN(__open64_2)

static int (*next_open)(const char*,int,...), (*next_open64)(const char*,int,...);
static int (*next_openat)(int,const char*,int,...), (*next_openat64)(int,const char*,int,...);
#define OPEN(name, params, args, directory) \
    int name params { \
        if (!next_##name) { errno=ENOSYS; return -1; } \
        mode_t mode=0; \
        if ((flags&O_CREAT) || (flags&O_TMPFILE)==O_TMPFILE) { \
            va_list values; va_start(values,flags); mode=va_arg(values,mode_t); va_end(values); \
        } \
        char absolute[PATH_MAX],mapped[PATH_MAX],followed[PATH_MAX],cloud[PATH_MAX],cloud_target[PATH_MAX]; \
        path=SandboxAtPath(directory,path,absolute,mapped); \
        if(CloudLinkTarget(path,cloud_target)>=0) { \
            if((flags&O_CREAT)&&(flags&O_EXCL)){errno=EEXIST;return -1;} \
            if(flags&O_NOFOLLOW){errno=ELOOP;return -1;} \
        } \
        path=FollowCloud(path,cloud,1); \
        if ((flags&O_CREAT)&&(flags&O_EXCL)&&SingletonName(path)) { \
            struct stat st; if(!next_lstat(path,&st)){errno=EEXIST;return -1;} \
        } \
        if ((flags&O_NOFOLLOW)&&SingletonName(path)) { \
            char target[PATH_MAX];if(SingletonTarget(path,target)>=0){errno=ELOOP;return -1;} \
        } \
        path=FollowSingleton(path,followed); \
        int value=next_##name args; int saved=errno; \
        TraceProcAccess(#name,path,value<0,saved); errno=saved; return value; \
    }
OPEN(open,(const char *path,int flags,...),(path,flags,mode),AT_FDCWD)
OPEN(open64,(const char *path,int flags,...),(path,flags,mode),AT_FDCWD)
OPEN(openat,(int fd,const char *path,int flags,...),(fd,path,flags,mode),fd)
OPEN(openat64,(int fd,const char *path,int flags,...),(fd,path,flags,mode),fd)

static int (*next_rename)(const char*,const char*);
static int (*next_renameat)(int,const char*,int,const char*);
int rename(const char *from,const char *to) {
    if(!next_rename){errno=ENOSYS;return -1;}
    char a[PATH_MAX],b[PATH_MAX],ca[PATH_MAX],cb[PATH_MAX];
    return next_rename(FollowCloud(SandboxPath(from,a),ca,0),FollowCloud(SandboxPath(to,b),cb,0));
}
int renameat(int fromfd,const char *from,int tofd,const char *to) {
    if(!next_renameat){errno=ENOSYS;return -1;}
    char a[PATH_MAX],b[PATH_MAX],aa[PATH_MAX],bb[PATH_MAX],ca[PATH_MAX],cb[PATH_MAX];
    return next_renameat(fromfd,FollowCloud(SandboxAtPath(fromfd,from,aa,a),ca,0),
                         tofd,FollowCloud(SandboxAtPath(tofd,to,bb,b),cb,0));
}
static ssize_t NativeExecutable(const char *path,char *buffer,size_t capacity) {
    const char *executable=getenv("STEAMOHOS_EXEC_PATH");
    if(!path || !executable || executable[0]!='/' || strcmp(path,"/proc/self/exe")) return -2;
    if(!capacity){errno=EINVAL;return -1;}
    if(!buffer){errno=EFAULT;return -1;}
    size_t length=strlen(executable);if(length>capacity)length=capacity;
    memcpy(buffer,executable,length);return length;
}
ssize_t readlink(const char *path,char *buffer,size_t capacity) {
    ssize_t result=NativeExecutable(path,buffer,capacity);if(result!=-2)return result;
    if(!next_readlink){errno=ENOSYS;return -1;}
    char mapped[PATH_MAX],target[PATH_MAX];path=SandboxPath(path,mapped);
    int length=CloudLinkTarget(path,target);if(length<0)length=SingletonTarget(path,target);
    if(length>=0){if(!capacity){errno=EINVAL;return -1;}
        size_t count=(size_t)length<capacity?(size_t)length:capacity;memcpy(buffer,target,count);return count;}
    char cloud[PATH_MAX];path=FollowCloud(path,cloud,0);
    result=next_readlink(path,buffer,capacity);int saved=errno;
    TraceProcAccess("readlink",path,result<0,saved);errno=saved;return result;
}
ssize_t readlinkat(int fd,const char *path,char *buffer,size_t capacity) {
    ssize_t result=NativeExecutable(path,buffer,capacity);if(result!=-2)return result;
    if(!next_readlinkat){errno=ENOSYS;return -1;}
    char absolute[PATH_MAX],parent[PATH_MAX];
    if(path[0]!='/'&&fd!=AT_FDCWD){
        char descriptor[64];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
        ssize_t length=next_readlink(descriptor,parent,sizeof(parent)-1);
        if(length>=0){parent[length]=0;int n=snprintf(absolute,sizeof(absolute),"%s/%s",parent,path);
            if(n>0&&n<(int)sizeof(absolute))return readlink(absolute,buffer,capacity);}
    }
    if(path[0]=='/'||fd==AT_FDCWD)return readlink(path,buffer,capacity);
    char mapped[PATH_MAX];return next_readlinkat(fd,SandboxPath(path,mapped),buffer,capacity);
}

static int (*next_mkstemp)(char*), (*next_mkstemp64)(char*);
static int (*next_mkstemps)(char*,int), (*next_mkstemps64)(char*,int);
static int (*next_mkostemp)(char*,int), (*next_mkostemp64)(char*,int);
static char *(*next_mkdtemp)(char*);
// Translate only the working copy: callers retain the original logical path
// and the six generated characters, without requiring a larger caller buffer.
#define TEMPFILE(name, params, arguments, suffix_count) \
    int name params { \
        if(!next_##name){errno=ENOSYS;return -1;} \
        char mapped[PATH_MAX]; const char *translated=SandboxPath(pattern,mapped); \
        if(translated==pattern)return next_##name arguments; \
        char *original=pattern;pattern=mapped; \
        int result=next_##name arguments; \
        if(result>=0){size_t length=strlen(original);size_t extension=(size_t)(suffix_count); \
            if(length>=6+extension)memcpy(original+length-6-extension,mapped+strlen(mapped)-6-extension,6);} \
        return result; \
    }
TEMPFILE(mkstemp,(char *pattern),(pattern),0)
TEMPFILE(mkstemp64,(char *pattern),(pattern),0)
TEMPFILE(mkstemps,(char *pattern,int suffix),(pattern,suffix),suffix)
TEMPFILE(mkstemps64,(char *pattern,int suffix),(pattern,suffix),suffix)
TEMPFILE(mkostemp,(char *pattern,int flags),(pattern,flags),0)
TEMPFILE(mkostemp64,(char *pattern,int flags),(pattern,flags),0)
char *mkdtemp(char *pattern) {
    if(!next_mkdtemp){errno=ENOSYS;return NULL;}
    char mapped[PATH_MAX];const char *translated=SandboxPath(pattern,mapped);
    if(translated==pattern)return next_mkdtemp(pattern);
    if(!next_mkdtemp(mapped))return NULL;
    size_t length=strlen(pattern);if(length>=6)memcpy(pattern+length-6,mapped+strlen(mapped)-6,6);
    return pattern;
}

static void InitializeFontconfig(void);
__attribute__((constructor)) static void InitializeFilesystem(void) {
#define RESOLVE(name) next_##name=dlsym(RTLD_NEXT,#name)
    RESOLVE(chdir);RESOLVE(mknod);RESOLVE(mknodat);RESOLVE(__xmknod);RESOLVE(__xmknodat);
    RESOLVE(mkdir);
    RESOLVE(mkdirat);
    RESOLVE(stat);
    RESOLVE(lstat);
    RESOLVE(stat64);
    RESOLVE(lstat64);
    RESOLVE(__xstat);
    RESOLVE(__lxstat);
    RESOLVE(__xstat64);
    RESOLVE(__lxstat64);
    RESOLVE(fstatat);
    RESOLVE(fstatat64);
    RESOLVE(access);
    RESOLVE(faccessat);
    RESOLVE(chmod);RESOLVE(utime);RESOLVE(utimes);
    RESOLVE(chown);
    RESOLVE(lchown);
    RESOLVE(unlink);
    RESOLVE(unlinkat);
    RESOLVE(rmdir);
    RESOLVE(mkfifo);
    RESOLVE(statfs);
    RESOLVE(statfs64);
    RESOLVE(statvfs);
    RESOLVE(statvfs64);
    RESOLVE(fopen);
    RESOLVE(fopen64);
    RESOLVE(freopen);
    RESOLVE(freopen64);
    RESOLVE(opendir);RESOLVE(scandir);RESOLVE(scandir64);RESOLVE(scandirat);RESOLVE(scandirat64);
    RESOLVE(__open_2);
    RESOLVE(__open64_2);
    RESOLVE(open);
    RESOLVE(open64);
    RESOLVE(openat);
    RESOLVE(openat64);
    RESOLVE(rename);
    RESOLVE(renameat);
    RESOLVE(readlink);
    RESOLVE(readlinkat);
    RESOLVE(mkstemp);RESOLVE(mkstemp64);RESOLVE(mkstemps);RESOLVE(mkstemps64);
    RESOLVE(mkostemp);RESOLVE(mkostemp64);RESOLVE(mkdtemp);
    dlerror();
    InitializeSingleLibrary();
    InitializeSingletonProfile();
    InitializeFontconfig();
}

// POSIX names share a real mmap-able inode in the app-private IPC namespace.
// Keep O_EXCL, unlink-while-mapped and cross-process visibility semantics.
static int (*next_shm_open)(const char*,int,mode_t);
static int (*next_shm_unlink)(const char*);
static int PrivateShmDirectory(const char *name, const char **leaf) {
    if(!name){errno=EINVAL;return -1;}
    while(*name=='/')++name;
    size_t length=strnlen(name,NAME_MAX+1);
    if(!length||strchr(name,'/')||!strcmp(name,".")||!strcmp(name,"..")){errno=EINVAL;return -1;}
    if(length>NAME_MAX){errno=ENAMETOOLONG;return -1;}
    const char *root=getenv("STEAMOHOS_IPC_ROOT");
    char directory[PATH_MAX];
    if(!root||root[0]!='/'||snprintf(directory,sizeof(directory),"%s/posix-shm",root)>=(int)sizeof(directory)){
        errno=EINVAL;return -1;
    }
    if(!next_mkdir||!next_open){errno=ENOSYS;return -1;}
    if(next_mkdir(directory,0700)&&errno!=EEXIST)return -1;
    int fd=next_open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)){int error=errno;close(fd);errno=error;return -1;}
    if(st.st_uid!=geteuid()||(st.st_mode&0077)){
        close(fd);errno=EACCES;return -1;
    }
    *leaf=name;return fd;
}
int shm_open(const char *name,int flags,mode_t mode) {
    if(!getenv("STEAMOHOS_IPC_ROOT")) {
        if(!next_shm_open)next_shm_open=dlsym(RTLD_NEXT,"shm_open");
        if(!next_shm_open){errno=ENOSYS;return -1;}return next_shm_open(name,flags,mode);
    }
    const char *leaf=NULL;int directory=PrivateShmDirectory(name,&leaf);if(directory<0)return -1;
    int fd=next_openat(directory,leaf,flags|O_NOFOLLOW|O_CLOEXEC,mode);
    int error=errno;close(directory);errno=error;
    if(fd<0&&error!=EEXIST&&error!=ENOENT)fprintf(stderr,"STEAMOHOS_SHM_OPEN_FAILED name=%s errno=%d\n",name,error);
    return fd;
}
int shm_unlink(const char *name) {
    if(!getenv("STEAMOHOS_IPC_ROOT")) {
        if(!next_shm_unlink)next_shm_unlink=dlsym(RTLD_NEXT,"shm_unlink");
        if(!next_shm_unlink){errno=ENOSYS;return -1;}return next_shm_unlink(name);
    }
    const char *leaf=NULL;int directory=PrivateShmDirectory(name,&leaf);if(directory<0)return -1;
    int result=next_unlinkat(directory,leaf,0);int error=errno;close(directory);errno=error;return result;
}

static int XmlPath(const char *path,char *escaped,size_t capacity) {
    size_t used=0;
    for(;*path;++path) {
        const char *replacement=NULL;
        if(*path=='&')replacement="&amp;";else if(*path=='<')replacement="&lt;";else if(*path=='>')replacement="&gt;";
        size_t count=replacement?strlen(replacement):1;
        if(used+count>=capacity)return 0;
        if(replacement)memcpy(escaped+used,replacement,count);else escaped[used]=*path;
        used+=count;
    }
    escaped[used]=0;return 1;
}
static void InitializeFontconfig(void) {
    const char *data=getenv("STEAMOHOS_STEAM_ROOT"),*temporary=getenv("STEAMOHOS_TMP_ROOT");
    if(!data||!temporary||getenv("FONTCONFIG_FILE"))return;
    char path[PATH_MAX],cache[PATH_MAX];
    if(snprintf(path,sizeof(path),"%s/steamohos-fonts.conf",temporary)>=(int)sizeof(path)||
       snprintf(cache,sizeof(cache),"%s/fontconfig-cache",temporary)>=(int)sizeof(cache))return;
    if(next_mkdir(cache,0700)&&errno!=EEXIST)return;
    char escaped_data[PATH_MAX*5],escaped_cache[PATH_MAX*5];
    if(!XmlPath(data,escaped_data,sizeof(escaped_data))||!XmlPath(cache,escaped_cache,sizeof(escaped_cache)))return;
    char staging[PATH_MAX];
    if(snprintf(staging,sizeof(staging),"%s.XXXXXX",path)>=(int)sizeof(staging))return;
    int fd=next_mkostemp(staging,O_CLOEXEC);if(fd<0)return;
    FILE *file=fdopen(fd,"w");if(!file){close(fd);next_unlink(staging);return;}
    int result=fprintf(file,"<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">"
        "<fontconfig><dir>%s/clientui/fonts</dir><dir>%s/resource/fonts</dir><cachedir>%s</cachedir>"
        "<config><rescan><int>0</int></rescan></config></fontconfig>\n",escaped_data,escaped_data,escaped_cache);
    int closed=fclose(file);
    if(result>0&&!closed&&!next_rename(staging,path))setenv("FONTCONFIG_FILE",path,0);
    else next_unlink(staging);
}


// The app sandbox forbids real symlinks even in private storage. Represent only
// Chromium's reserved singleton links with exclusive, locked metadata files.
// Ordinary symlink calls retain their real platform result.
static const char SingletonMagic[]="STEAMOHOS_CEF_SINGLETON_V1\n";
static int SingletonName(const char *path) {
    if(!path||path[0]!='/')return 0;
    const char *leaf=strrchr(path,'/');if(!leaf)return 0;++leaf;
    int kind=!strcmp(leaf,"SingletonLock")?1:!strcmp(leaf,"SingletonCookie")?2:!strcmp(leaf,"SingletonSocket")?3:0;
    if(!kind)return 0;
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT"),*tmp=getenv("STEAMOHOS_TMP_ROOT");
    char profile[PATH_MAX];
    if(ipc&&snprintf(profile,sizeof(profile),"%s/cef-profile",ipc)>0&&Below(path,profile)&&
       (size_t)(leaf-path)==strlen(profile)+1)return kind;
    if(tmp&&Below(path,tmp))return kind;
    return 0;
}
static void InitializeSingletonProfile(void) {
    const char *root=getenv("STEAMOHOS_IPC_ROOT");char path[PATH_MAX];
    if(root&&snprintf(path,sizeof(path),"%s/cef-profile",root)>0)next_mkdir(path,0700);
}
static int SingletonGuard(void) {
    const char *root=getenv("STEAMOHOS_IPC_ROOT");char path[PATH_MAX];
    if(!root||snprintf(path,sizeof(path),"%s/cef-singleton.guard",root)>=(int)sizeof(path)){errno=EINVAL;return -1;}
    int fd=next_open(path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)){
        close(fd);errno=EACCES;return -1;
    }
    int result;do{result=flock(fd,LOCK_EX);}while(result&&errno==EINTR);
    if(result){int error=errno;close(fd);errno=error;return -1;}return fd;
}
static int SingletonTarget(const char *path,char target[PATH_MAX]) {
    if(!SingletonName(path))return -1;
    int guard=SingletonGuard();if(guard<0)return -1;
    int fd=next_open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){close(guard);return -1;}
    char content[sizeof(SingletonMagic)+PATH_MAX];
    ssize_t count=read(fd,content,sizeof(content));close(fd);close(guard);
    size_t magic=sizeof(SingletonMagic)-1;
    if(count<=(ssize_t)magic||count>=(ssize_t)(magic+PATH_MAX)||memcmp(content,SingletonMagic,magic))return -1;
    size_t length=(size_t)count-magic;memcpy(target,content+magic,length);target[length]=0;
    return (int)length;
}
static const char *FollowSingleton(const char *path,char followed[PATH_MAX]) {
    if(!path)return path;
    char target[PATH_MAX];if(SingletonTarget(path,target)<0)return path;
    if(target[0]=='/'){
        char mapped[PATH_MAX];const char *value=SandboxPath(target,mapped);
        if(strlen(value)>=PATH_MAX){return path;}strcpy(followed,value);return followed;
    }
    const char *slash=strrchr(path,'/');if(!slash)return path;
    int size=snprintf(followed,PATH_MAX,"%.*s/%s",(int)(slash-path),path,target);
    return size>0&&size<PATH_MAX?followed:path;
}
static int (*next_symlink)(const char*,const char*);
static int (*next_symlinkat)(const char*,int,const char*);
int symlink(const char *target,const char *linkpath) {
    char mapped[PATH_MAX];linkpath=SandboxPath(linkpath,mapped);
    int kind=SingletonName(linkpath);
    if(!kind){if(!next_symlink)next_symlink=dlsym(RTLD_NEXT,"symlink");
        if(!next_symlink){errno=ENOSYS;return -1;}return next_symlink(target,linkpath);}
    size_t length=strnlen(target,PATH_MAX);
    if(!length||length>=PATH_MAX){errno=length?ENAMETOOLONG:ENOENT;return -1;}
    if(kind!=3&&strchr(target,'/')){errno=ENOTSUP;return -1;}
    if(kind==3){
        char translated[PATH_MAX],absolute[PATH_MAX];
        const char *value=SandboxPath(target,translated),*tmp=getenv("STEAMOHOS_TMP_ROOT");
        if(!tmp||!AbsolutePath(value,absolute)||!Below(absolute,tmp)){errno=ENOTSUP;return -1;}
    }
    int guard=SingletonGuard();if(guard<0)return -1;
    int fd=next_open(linkpath,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){int error=errno;close(guard);errno=error;return -1;}
    char content[sizeof(SingletonMagic)+PATH_MAX];size_t magic=sizeof(SingletonMagic)-1;
    memcpy(content,SingletonMagic,magic);memcpy(content+magic,target,length);
    size_t used=0,total=magic+length;
    while(used<total){ssize_t count=write(fd,content+used,total-used);
        if(count<0&&errno==EINTR)continue;
        if(count<=0){int error=count<0?errno:EIO;close(fd);next_unlink(linkpath);close(guard);errno=error;return -1;}
        used+=(size_t)count;
    }
    close(fd);close(guard);return 0;
}
int symlinkat(const char *target,int fd,const char *linkpath) {
    if(linkpath[0]=='/'||fd==AT_FDCWD)return symlink(target,linkpath);
    char descriptor[64],parent[PATH_MAX],absolute[PATH_MAX];
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    ssize_t length=next_readlink(descriptor,parent,sizeof(parent)-1);
    if(length>=0){parent[length]=0;int n=snprintf(absolute,sizeof(absolute),"%s/%s",parent,linkpath);
        if(n>0&&n<(int)sizeof(absolute)&&SingletonName(absolute))return symlink(target,absolute);}
    if(!next_symlinkat)next_symlinkat=dlsym(RTLD_NEXT,"symlinkat");
    if(!next_symlinkat){errno=ENOSYS;return -1;}return next_symlinkat(target,fd,linkpath);
}
// Used only by this GNU preload's AF_UNIX adapter, never by the Wine host.
int SteamOhosUnixSocketPath(const char *path,char *mapped,size_t capacity) {
    char temporary[PATH_MAX],followed[PATH_MAX];
    const char *value=FollowSingleton(SandboxPath(path,temporary),followed);
    if(!strcmp(value,path))return 0;
    size_t length=strlen(value);if(length>=capacity){errno=ENAMETOOLONG;return -1;}
    memcpy(mapped,value,length+1);return 1;
}

static const char *AtPath(int fd,const char *path,char absolute[PATH_MAX]) {
    if(!path||path[0]=='/'||fd==AT_FDCWD||!*path)return path;
    char descriptor[64],parent[PATH_MAX];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    ssize_t count=next_readlink(descriptor,parent,sizeof(parent)-1);
    if(count<0){return path;}parent[count]=0;
    int size=snprintf(absolute,PATH_MAX,"%s/%s",parent,path);
    return size>0&&size<PATH_MAX?absolute:path;
}
static const char *SandboxAtPath(int fd,const char *path,char absolute[PATH_MAX],char mapped[PATH_MAX]) {
    const char *resolved=AtPath(fd,path,absolute);
    const char *translated=SandboxPath(resolved,mapped);
    // Preserve real dirfd semantics for ordinary paths, including /proc magic
    // links and open-but-deleted directories. Only our explicit aliases change.
    char link[PATH_MAX],target[PATH_MAX];const char *suffix;
    return strcmp(resolved,translated)||SingletonName(translated)||CloudLinkPath(translated,link,target,&suffix)?translated:path;
}

// Bounded GNU-only diagnostics: report the actual dirfd and proc magic-link
// target on failure. Never retry with weaker flags or manufacture a stat result.
static void TraceAtFailure(int fd,const char *path,int flags,int error) {
    const char *enabled=getenv("STEAMOHOS_NATIVE_MEMORY");
    if(!enabled||strcmp(enabled,"1")||fd<0||!path||path[0]=='/')return;
    static unsigned reports;
    if(__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)>=24)return;
    char descriptor[64],directory[PATH_MAX],target[PATH_MAX];
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    ssize_t n=next_readlink?next_readlink(descriptor,directory,sizeof(directory)-1):-1;
    if(n<0)strcpy(directory,"<unavailable>");else directory[n]=0;
    n=next_readlinkat?next_readlinkat(fd,path,target,sizeof(target)-1):-1;
    if(n<0)strcpy(target,"<unavailable>");else target[n]=0;
    struct stat status;int descriptor_ok=!fstat(fd,&status);
    fprintf(stderr,"STEAMOHOS_FSTATAT_FAILURE pid=%d fd=%d path=%.160s flags=0x%x errno=%d directory=%.360s target=%.360s fd_valid=%d\n",
        getpid(),fd,path,flags,error,directory,target,descriptor_ok);
    errno=error;
}

// Diagnostic only: observe real /proc results in the original lsof process.
// Do not alter permissions, output records, return values or errno.
static void TraceProcAccess(const char *operation,const char *path,int failed,int error) {
    const char *image=getenv("STEAMOHOS_EXEC_PATH"),*native=getenv("STEAMOHOS_NATIVE_MEMORY");
    if(!image||!native||strcmp(native,"1")||!path||strncmp(path,"/proc",5)||
       (path[5]&&path[5]!='/'))return;
    const char *leaf=strrchr(image,'/');
    if(!leaf||strcmp(leaf,"/steamohos_lsof"))return;
    static unsigned reports,network_reports;
    int network=!strncmp(path,"/proc/net",9)||strstr(path,"/net/")!=NULL;
    if(network) {
        if(__atomic_fetch_add(&network_reports,1,__ATOMIC_RELAXED)>=16)return;
    } else if(!failed||__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)>=8)return;
    char target[256]={0};
    if(failed&&strstr(path,"/fd/")&&next_readlink) {
        ssize_t n=next_readlink(path,target,sizeof(target)-1);if(n>=0)target[n]=0;
    }
    fprintf(stderr,"STEAMOHOS_PROC_ACCESS pid=%d operation=%s failed=%d errno=%d path=%.240s target=%.240s\n",
        getpid(),operation,failed,failed?error:0,path,target);
}


// Steam's native Proton Cloud resolver reads compatdata/<appid>/pfx directly;
// getnativepath is not used for its Windows known-folder roots. Keep only the
// compatibility metadata private. Installed games and client data remain on Z:.
// OHOS forbids symlink creation. Two validated private link records provide
// scoped path traversal, lstat/readlink and no-follow cleanup semantics without
// changing the real Wine profile. Never map pfx onto the entire live prefix.
static int CloudError(const char *stage) {
    int error=errno;
    fprintf(stderr,"STEAMOHOS_CLOUD_PREPARE stage=%s errno=%d\n",stage,error);
    errno=error;return -1;
}
static int CloudAppId(const char *text,size_t length) {
    if(!length||length>10||text[0]=='0')return 0;
    unsigned long long value=0;
    for(size_t i=0;i<length;++i) {
        if(text[i]<'0'||text[i]>'9')return 0;
        value=value*10+(unsigned)(text[i]-'0');
    }
    return value<=4294967295ULL;
}
static const char *CloudCompatPath(const char *path,char mapped[PATH_MAX]) {
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT"),*prefix=getenv("STEAMOHOS_WINE_PREFIX");
    if(!path||!*path||!ipc||ipc[0]!='/'||!prefix||prefix[0]!='/')return path;
    char absolute[PATH_MAX];if(!AbsolutePath(path,absolute))return path;
    const char *roots[]={getenv("STEAMOHOS_LIBRARY_ROOT"),getenv("STEAMOHOS_STEAM_ROOT")};
    for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);++i) {
        if(!roots[i]||roots[i][0]!='/')continue;
        char alias[PATH_MAX];int n=snprintf(alias,sizeof(alias),"%s/steamapps/compatdata",roots[i]);
        if(n<=0||n>=(int)sizeof(alias)||!Below(absolute,alias))continue;
        const char *relative=absolute+n;
        if(*relative) {
            const char *id=relative+1,*end=strchr(id,'/');
            size_t length=end?(size_t)(end-id):strlen(id);
            if(!CloudAppId(id,length))return path;
        }
        n=snprintf(mapped,PATH_MAX,"%s/hmosdeck-cloud-compatdata%s",ipc,relative);
        return n>0&&n<PATH_MAX?mapped:path;
    }
    return path;
}
static int CloudPrivateDirectory(const char *path) {
    struct stat status;
    if(!next_mkdir(path,0700))return 1;
    return errno==EEXIST&&!next_lstat(path,&status)&&S_ISDIR(status.st_mode)&&status.st_uid==getuid();
}

static const char CloudMagic[]="STEAMOHOS_CLOUD_LINK_V1\n";
static int CloudReadRecord(const char *path,const char *target) {
    int fd=next_open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    struct stat status;char bytes[sizeof(CloudMagic)+PATH_MAX];ssize_t n=read(fd,bytes,sizeof(bytes));
    int valid=!fstat(fd,&status)&&S_ISREG(status.st_mode)&&status.st_uid==getuid();close(fd);
    size_t magic=sizeof(CloudMagic)-1,length=strlen(target);
    return valid&&n==(ssize_t)(magic+length)&&!memcmp(bytes,CloudMagic,magic)&&!memcmp(bytes+magic,target,length);
}
static int CloudLink(const char *target,const char *path) {
    int fd=next_open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return errno==EEXIST&&CloudReadRecord(path,target);
    char bytes[sizeof(CloudMagic)+PATH_MAX];size_t magic=sizeof(CloudMagic)-1,length=strlen(target);
    memcpy(bytes,CloudMagic,magic);memcpy(bytes+magic,target,length);size_t used=0,total=magic+length;
    while(used<total) {
        ssize_t n=write(fd,bytes+used,total-used);if(n<0&&errno==EINTR)continue;
        if(n<=0){int error=n<0?errno:EIO;close(fd);next_unlink(path);errno=error;return 0;}
        used+=(size_t)n;
    }
    close(fd);return 1;
}
static int CloudCreatePrefix(const char *base,const char *id,const char *profile,const char *prefix) {
    const char *suffixes[]={"","/pfx","/pfx/drive_c","/pfx/drive_c/users"};
    char path[PATH_MAX],target[PATH_MAX];
    for(unsigned i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);++i) {
        int n=snprintf(path,sizeof(path),"%s/%s%s",base,id,suffixes[i]);
        if(n<=0||n>=(int)sizeof(path)||!CloudPrivateDirectory(path))return 0;
    }
    int n=snprintf(path,sizeof(path),"%s/%s/pfx/drive_c/users/steamuser",base,id);
    if(n<=0||n>=(int)sizeof(path)||!CloudLink(profile,path))return 0;
    n=snprintf(path,sizeof(path),"%s/%s/pfx/drive_c/ProgramData",base,id);
    int m=snprintf(target,sizeof(target),"%s/drive_c/ProgramData",prefix);
    return n>0&&n<(int)sizeof(path)&&m>0&&m<(int)sizeof(target)&&CloudLink(target,path);
}
// Called once by the GNU desktop session before starting Steam, never lazily
// during filesystem operations (which could recreate a tree during cleanup).
int SteamOhosPrepareCloudPrefixes(void) {
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT"),*prefix=getenv("STEAMOHOS_WINE_PREFIX");
    if(!ipc||ipc[0]!='/'||!prefix||prefix[0]!='/')return 0;
    char base[PATH_MAX],profile[PATH_MAX];struct stat status;
    int n=snprintf(base,sizeof(base),"%s/hmosdeck-cloud-compatdata",ipc);
    int m=snprintf(profile,sizeof(profile),"%s/drive_c/users/steamuser",prefix);
    if(n<=0||n>=(int)sizeof(base)||m<=0||m>=(int)sizeof(profile)||
       next_stat(profile,&status)||!S_ISDIR(status.st_mode))return CloudError("profile");
    if(!CloudPrivateDirectory(base))return CloudError("private-metadata");
    const char *roots[]={getenv("STEAMOHOS_LIBRARY_ROOT"),getenv("STEAMOHOS_STEAM_ROOT")};
    int prepared=0;
    for(unsigned i=0;i<sizeof(roots)/sizeof(roots[0]);++i) {
        if(single_library&&i==1)continue;
        if(!roots[i]||roots[i][0]!='/')continue;
        char apps[PATH_MAX];n=snprintf(apps,sizeof(apps),"%s/steamapps",roots[i]);
        if(n<=0||n>=(int)sizeof(apps))return -1;
        DIR *directory=next_opendir(apps);if(!directory)continue;
        struct dirent *entry;unsigned inspected=0;
        while((entry=readdir(directory))&&++inspected<=4096) {
            const char *name=entry->d_name;size_t length=strlen(name);
            if(length<17||strncmp(name,"appmanifest_",12)||strcmp(name+length-4,".acf")||
               !CloudAppId(name+12,length-16))continue;
            char id[11];memcpy(id,name+12,length-16);id[length-16]=0;
            if(!CloudCreatePrefix(base,id,profile,prefix)){int error=errno;closedir(directory);errno=error;return CloudError("prefix-layout");}
            ++prepared;
        }
        closedir(directory);
    }
    fprintf(stderr,"STEAMOHOS_CLOUD_PREFIXES prepared=%d scoped_path_links=1\n",prepared);
    return prepared;
}

// Recognize only the configured user-profile and ProgramData metadata links.
static int CloudLinkPath(const char *path,char link[PATH_MAX],char target[PATH_MAX],const char **suffix) {
    const char *ipc=getenv("STEAMOHOS_IPC_ROOT"),*prefix=getenv("STEAMOHOS_WINE_PREFIX");
    if(!ipc||ipc[0]!='/'||!prefix||prefix[0]!='/'||!path)return 0;
    char base[PATH_MAX];int n=snprintf(base,sizeof(base),"%s/hmosdeck-cloud-compatdata",ipc);
    if(n<=0||n>=(int)sizeof(base)||!Below(path,base)||path[n]!='/')return 0;
    const char *id=path+n+1,*end=strchr(id,'/');if(!end||!CloudAppId(id,(size_t)(end-id)))return 0;
    const char *logical[]={"/pfx/drive_c/users/steamuser","/pfx/drive_c/ProgramData"};
    const char *native[]={"/drive_c/users/steamuser","/drive_c/ProgramData"};
    for(unsigned i=0;i<2;++i)if(Below(end,logical[i])) {
        size_t length=(size_t)(end-path)+strlen(logical[i]);if(length>=PATH_MAX)return 0;
        memcpy(link,path,length);link[length]=0;
        n=snprintf(target,PATH_MAX,"%s%s",prefix,native[i]);if(n<=0||n>=PATH_MAX)return 0;
        *suffix=path+length;return i+1;
    }
    return 0;
}
static int CloudLinkTarget(const char *path,char target[PATH_MAX]) {
    char link[PATH_MAX],absolute[PATH_MAX];const char *suffix;
    if(!path||!AbsolutePath(path,absolute))return -1;
    if(!CloudLinkPath(absolute,link,target,&suffix)||*suffix||!CloudReadRecord(link,target))return -1;
    return (int)strlen(target);
}
static const char *FollowCloud(const char *path,char followed[PATH_MAX],int follow_leaf) {
    if(!path)return path;
    char absolute[PATH_MAX],link[PATH_MAX],target[PATH_MAX];const char *suffix;
    if(!AbsolutePath(path,absolute))return path;
    int kind=CloudLinkPath(absolute,link,target,&suffix);
    if(!kind||(!follow_leaf&&!*suffix)||!CloudReadRecord(link,target))return path;
    const char *tail=suffix,*canonical="";
    if(kind==1) {
        // Current Wine registry uses Documents and AppData/{Local,Roaming};
        // Steam's built-in Proton resolver requests these legacy spellings.
        const char *legacy[]={"/My Documents","/Application Data","/Local Settings/Application Data","/Local Settings"};
        const char *modern[]={"/Documents","/AppData/Roaming","/AppData/Local","/AppData"};
        for(unsigned i=0;i<4;++i)if(Below(suffix,legacy[i])) {
            canonical=modern[i];tail=suffix+strlen(legacy[i]);break;
        }
    }
    int n=snprintf(followed,PATH_MAX,"%s%s%s",target,canonical,tail);
    return n>0&&n<PATH_MAX?followed:path;
}
char *realpath(const char *path,char *resolved) {
    static char *(*next_realpath)(const char*,char*);
    if(!next_realpath)next_realpath=dlsym(RTLD_NEXT,"realpath");
    if(!next_realpath){errno=ENOSYS;return NULL;}
    char mapped[PATH_MAX],cloud[PATH_MAX];
    return next_realpath(FollowCloud(SandboxPath(path,mapped),cloud,1),resolved);
}

/* Fortified GNU callers do not enter the public realpath symbol. Delegate to
 * libc's checked implementation after mapping so its size/overflow checks and
 * error behavior remain intact. */
char *__realpath_chk(const char *path,char *resolved,size_t capacity) {
    static char *(*next_checked)(const char*,char*,size_t);
    if(!next_checked)next_checked=dlsym(RTLD_NEXT,"__realpath_chk");
    if(!next_checked){errno=ENOSYS;return NULL;}
    char mapped[PATH_MAX],cloud[PATH_MAX];
    return next_checked(FollowCloud(SandboxPath(path,mapped),cloud,1),resolved,capacity);
}
