/* ArkUI <-> GNU supervisor control. Only fixed Steam actions cross this boundary.
 * An ACK confirms URI dispatch, never authentication, game launch or Cloud success. */
struct deck_control {
    char directory[PATH_MAX], generation[65], client[PATH_MAX];
    unsigned last, pending;
    pid_t child;
    uint64_t deadline, next_state;
    int started, closed, last_mode;
};
static int deck_token(const char *s) {
    size_t n=strnlen(s,65);
    return n>0&&n<=64&&strspn(s,"0123456789abcdef-")==n;
}
#include "deck_action_uri.h"
static void deck_write(struct deck_control *c,const char *name,const char *text) {
    char path[PATH_MAX],tmp[PATH_MAX];
    if(snprintf(path,sizeof(path),"%s/%s",c->directory,name)>=(int)sizeof(path)||
       snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return;
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return;
    size_t length=strlen(text);ssize_t n=write(fd,text,length);close(fd);
    if(n==(ssize_t)length)rename(tmp,path);
}
static void deck_ack(struct deck_control *c,unsigned id,const char *status,int code) {
    char text[256];
    snprintf(text,sizeof(text),"{\"version\":1,\"generation\":\"%s\",\"requestId\":%u,\"status\":\"%s\",\"exitCode\":%d}\n",
        c->generation,id,status,code);
    deck_write(c,"deck-command-ack.json",text);
}
static int deck_initialize(struct deck_control *c) {
    memset(c,0,sizeof(*c));
    const char *generation=getenv("HMOSDECK_SESSION_GENERATION"),*tmp=getenv("STEAMOHOS_TMP_ROOT"),*code=getenv("STEAMOHOS_CODE_ROOT");
    if(!generation||!deck_token(generation)||!tmp||tmp[0]!='/'||!code||code[0]!='/')return 0;
    if(snprintf(c->directory,sizeof(c->directory),"%s",tmp)>=(int)sizeof(c->directory)||
       snprintf(c->client,sizeof(c->client),"%s/steamrtarm64/steam",code)>=(int)sizeof(c->client))return 0;
    strcpy(c->generation,generation);return 1;
}
static void deck_state(struct deck_control *c,int ready,int closed,int mode) {
    char text[256];
    snprintf(text,sizeof(text),"{\"version\":1,\"generation\":\"%s\",\"ready\":%s,\"closed\":%s,\"mode\":%d,\"updatedAt\":%llu}\n",
        c->generation,ready?"true":"false",closed?"true":"false",mode,(unsigned long long)time(NULL)*1000);
    deck_write(c,"deck-session-state.json",text);
}
static void deck_tick(struct deck_control *c,int ready,int mode) {
    if(c->closed)return;
    // Readiness describes a surface, not the supervised client's lifetime.
    // After a game or CEF withdraws it, navigation and shutdown must still work.
    // Before the first surface exists, keep the cold-start command barrier.
    if(ready){c->started=1;if(mode>=0&&mode<=2)c->last_mode=mode;}
    uint64_t current=now();
    if(current>=c->next_state){deck_state(c,ready,0,c->last_mode);c->next_state=current+1000;}
    if(c->child>0) {
        int status=0;pid_t done=waitpid(c->child,&status,WNOHANG);
        if(done==c->child||(done<0&&errno!=EINTR)) {
            int code=done==c->child&&WIFEXITED(status)?WEXITSTATUS(status):-1;
            deck_ack(c,c->pending,code==0?"dispatched":"failed",code);c->child=0;
        } else if(current>=c->deadline) {
            kill(c->child,SIGKILL);waitpid(c->child,NULL,0);c->child=0;
            deck_ack(c,c->pending,"timeout",-1);
        }
        return;
    }
    if(!c->started)return;
    char path[PATH_MAX],text[257];
    if(snprintf(path,sizeof(path),"%s/deck-command.txt",c->directory)>=(int)sizeof(path))return;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
    struct stat st;ssize_t n=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size>256?-1:read(fd,text,256);close(fd);
    if(n<=0)return;
    text[n]=0;
    unsigned version=0,id=0;char generation[65],action[25],app[12],extra;
    if(sscanf(text,"%u %64s %u %24s %11s %c",&version,generation,&id,action,app,&extra)!=5||
       version!=1||!deck_token(generation)||strcmp(generation,c->generation)||!id||id>2147483647U||id<=c->last)return;
    c->last=id;
    char uri[96];const char *value=deck_uri(action,app,uri,sizeof(uri));
    if(!value){deck_ack(c,id,"rejected",-1);return;}
    pid_t owner=getpid(),child=fork();
    if(child<0){deck_ack(c,id,"failed",errno);return;}
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=owner)_exit(126);
        char *args[]={c->client,(char*)value,NULL};execv(c->client,args);_exit(127);
    }
    c->child=child;c->pending=id;c->deadline=current+10000;
}
static void deck_finish(struct deck_control *c) {
    c->closed=1;
    if(c->child>0){kill(c->child,SIGKILL);waitpid(c->child,NULL,0);c->child=0;}
    deck_state(c,0,1,0);
}
