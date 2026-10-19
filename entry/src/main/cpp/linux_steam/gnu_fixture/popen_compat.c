// GNU-only popen: libc's hidden spawn exec cannot enter our external GNU loader.
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
struct ChildPipe { FILE *stream; pid_t pid; struct ChildPipe *next; };
static struct ChildPipe *children;
static pthread_mutex_t children_lock=PTHREAD_MUTEX_INITIALIZER;

static void ChildFailure(int fd,int error) {
    // The pipe is blocking and the errno record fits in PIPE_BUF.
    while(write(fd,&error,sizeof(error))<0 && errno==EINTR) {}
    _exit(127);
}

FILE *popen(const char *command,const char *mode) {
    const char *code=getenv("STEAMOHOS_CODE_ROOT");
    if(!code || code[0]!='/') {
        FILE *(*next)(const char*,const char*)=dlsym(RTLD_NEXT,"popen");
        if(!next){errno=ENOSYS;return NULL;}
        return next(command,mode);
    }
    if(!command || !mode || (strcmp(mode,"r") && strcmp(mode,"re") &&
        strcmp(mode,"w") && strcmp(mode,"we"))) {errno=EINVAL;return NULL;}
    int descriptors[2],errors[2];
    if(pipe2(descriptors,O_CLOEXEC))return NULL;
    if(pipe2(errors,O_CLOEXEC)) {
        int saved=errno;close(descriptors[0]);close(descriptors[1]);errno=saved;return NULL;
    }
    const int reading=mode[0]=='r';
    const int parent=descriptors[reading?0:1],child=descriptors[reading?1:0];
    struct ChildPipe *record=malloc(sizeof(*record));
    FILE *stream=record?fdopen(parent,reading?"r":"w"):NULL;
    if(!stream) {
        int saved=record?errno:ENOMEM;free(record);
        close(descriptors[0]);close(descriptors[1]);close(errors[0]);close(errors[1]);errno=saved;return NULL;
    }
    pthread_mutex_lock(&children_lock);
    pid_t pid=fork();
    if(pid==0) {
        close(parent);close(errors[0]);
        // POSIX requires closing the parent's other popen streams in this child.
        for(struct ChildPipe *item=children;item;item=item->next)close(fileno(item->stream));
        int target=reading?STDOUT_FILENO:STDIN_FILENO;
        if(child!=target) {
            if(dup2(child,target)<0)ChildFailure(errors[1],errno);
            close(child);
        } else if(fcntl(child,F_SETFD,0)<0)ChildFailure(errors[1],errno);
        char *arguments[]={"sh","-c","--",(char*)command,NULL};
        // The public GNU exec hook performs the verified native handoff. It
        // closes CLOEXEC descriptors only once the new image is prepared.
        execve("/bin/sh",arguments,environ);
        ChildFailure(errors[1],errno);
    }
    int saved=errno;
    close(child);close(errors[1]);
    if(pid<0) {
        pthread_mutex_unlock(&children_lock);
        fclose(stream);free(record);close(errors[0]);errno=saved;return NULL;
    }
    int child_error=0;ssize_t received;
    do {received=read(errors[0],&child_error,sizeof(child_error));}while(received<0 && errno==EINTR);
    saved=errno;close(errors[0]);
    if(received!=0 || (!strchr(mode,'e') && fcntl(parent,F_SETFD,0)<0)) {
        if(received>0)saved=received==sizeof(child_error)?child_error:EIO;
        else if(received==0)saved=errno;
        pthread_mutex_unlock(&children_lock);
        fclose(stream);free(record);
        while(waitpid(pid,NULL,0)<0 && errno==EINTR) {}
        errno=saved;return NULL;
    }
    record->stream=stream;record->pid=pid;record->next=children;children=record;
    pthread_mutex_unlock(&children_lock);
    return stream;
}

int pclose(FILE *stream) {
    pthread_mutex_lock(&children_lock);
    struct ChildPipe **link=&children;
    while(*link && (*link)->stream!=stream)link=&(*link)->next;
    struct ChildPipe *record=*link;
    if(record)*link=record->next;
    pthread_mutex_unlock(&children_lock);
    if(!record) {
        int (*next)(FILE*)=dlsym(RTLD_NEXT,"pclose");
        if(!next){errno=ENOSYS;return -1;}
        return next(stream);
    }
    pid_t pid=record->pid;free(record);
    int closed=fclose(stream),saved=errno,status=0;
    pid_t waited;
    do {waited=waitpid(pid,&status,0);}while(waited<0 && errno==EINTR);
    if(waited<0)return -1;
    if(closed){errno=saved;return -1;}
    return status;
}
