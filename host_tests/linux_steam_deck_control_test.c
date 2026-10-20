#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/deck_session_control.h"
static void put(const char *path,const char *text){FILE *f=fopen(path,"w");assert(f);fputs(text,f);fclose(f);}
static int has(const char *path,const char *text){char b[512]={0};FILE *f=fopen(path,"r");if(!f)return 0;size_t n=fread(b,1,sizeof(b)-1,f);fclose(f);return n>0&&strstr(b,text)!=NULL;}
static void drain(struct deck_control *c){for(int i=0;i<100&&c->child;++i){deck_tick(c,1,0);poll(NULL,0,10);}assert(!c->child);}
int main(int argc,char **argv){
    if(argc==2){put(getenv("DECK_TEST_DISPATCH"),argv[1]);return 0;}
    char tmp[]="/tmp/steam-deck-control-XXXXXX";assert(mkdtemp(tmp));
    char path[PATH_MAX],ack[PATH_MAX],sent[PATH_MAX];
    snprintf(path,sizeof(path),"%s/deck-command.txt",tmp);snprintf(ack,sizeof(ack),"%s/deck-command-ack.json",tmp);snprintf(sent,sizeof(sent),"%s/sent",tmp);
    setenv("HMOSDECK_SESSION_GENERATION","abcd-1234",1);setenv("STEAMOHOS_TMP_ROOT",tmp,1);setenv("STEAMOHOS_CODE_ROOT",tmp,1);setenv("DECK_TEST_DISPATCH",sent,1);
    struct deck_control c;assert(deck_initialize(&c));assert(realpath(argv[0],c.client));
    put(path,"1 abcd-1234 1 launch 3590\n");deck_tick(&c,0,0);assert(!c.child&&!c.last);
    deck_tick(&c,1,0);drain(&c);assert(has(ack,"\"requestId\":1"));assert(has(ack,"dispatched"));assert(has(sent,"steam://rungameid/3590"));
    put(sent,"unchanged");deck_tick(&c,1,0);assert(has(sent,"unchanged"));
    put(path,"1 old-generation 2 launch 3590\n");deck_tick(&c,1,0);assert(c.last==1&&!c.child);
    put(path,"1 abcd-1234 2 launch 4294967296\n");deck_tick(&c,1,0);assert(c.last==2&&has(ack,"rejected"));
    put(path,"1 abcd-1234 3 shell 0\n");deck_tick(&c,1,0);assert(c.last==3&&has(ack,"rejected"));
    put(path,"1 abcd-1234 4 store 0 extra\n");deck_tick(&c,1,0);assert(c.last==3);
    put(path,"1 abcd-1234 4 downloads 0\n");deck_tick(&c,1,0);drain(&c);assert(has(sent,"steam://open/downloads"));
    put(path,"1 abcd-1234 5 bigPicture 0\n");deck_tick(&c,1,2);drain(&c);assert(has(sent,"steam://open/bigpicture"));
    deck_state(&c,1,0,2);snprintf(path,sizeof(path),"%s/deck-session-state.json",tmp);assert(has(path,"\"mode\":2"));
    deck_finish(&c);assert(has(path,"\"closed\":true"));
    setenv("HMOSDECK_SESSION_GENERATION","../../bad",1);assert(!deck_initialize(&c));
    puts("Deck GNU control: readiness, fixed URI dispatch, ACK, replay, stale generation, invalid AppID, invalid action, presentation and exit PASS");return 0;
}
