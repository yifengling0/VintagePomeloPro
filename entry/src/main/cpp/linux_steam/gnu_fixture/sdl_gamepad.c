/* Native GNU SDL3 virtual controller fed by the existing OHOS ControllerHub.
 * Loaded only in the original Steam process; CEF/compatibility tools opt out.
 * The official SDL ELF is preserved. No /dev/uinput or system device changes. */
#define _GNU_SOURCE
#include "sdl3_virtual_abi.h"
#include "../../input/controller/gamepad_ipc_protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
static atomic_bool stopping=false;
static pthread_t worker;
static bool running;
static pthread_mutex_t connection=PTHREAD_MUTEX_INITIALIZER;
static int endpoint=-1;
static uint32_t (*was_init)(uint32_t);
static uint32_t (*attach_joystick)(const HmosVirtualJoystickDesc*);
static void *(*open_joystick)(uint32_t);
static void (*close_joystick)(void*);
static bool (*detach_joystick)(uint32_t);
static bool (*set_axis)(void*,int,int16_t);
static bool (*set_button)(void*,int,bool);
static bool (*set_hat)(void*,int,uint8_t);
static bool rumble(void *unused,uint16_t low,uint16_t high) {
    (void)unused;
    struct {struct whgp_header header;struct whgp_rumble_v1 body;} packet={
        {WHGP_MAGIC,WHGP_VERSION,WHGP_MSG_RUMBLE,0,sizeof(struct whgp_rumble_v1)},{low,high,0}};
    pthread_mutex_lock(&connection);
    bool sent=endpoint>=0&&send(endpoint,&packet,sizeof(packet),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(packet);
    pthread_mutex_unlock(&connection);return sent;
}
static int16_t invert_y(int16_t y){return y==INT16_MIN?INT16_MAX:(int16_t)-y;}
static int16_t trigger(uint16_t value){if(value>32767)value=32767;return (int16_t)((uint32_t)value*65535/32767-32768);}
static void update(void *joystick,const struct whgp_state_v2 *s) {
    set_axis(joystick,0,s->lx);set_axis(joystick,1,invert_y(s->ly));
    set_axis(joystick,2,s->rx);set_axis(joystick,3,invert_y(s->ry));
    set_axis(joystick,4,trigger(s->lt));
    set_axis(joystick,5,trigger(s->rt));
    // SDL gamepad buttons: South/East/West/North, Back/Guide/Start,
    // left/right stick, left/right shoulder. WHGP uses A/B/X/Y/LB/RB/Back/Start/L3/R3.
    static const unsigned whgp_bit[]={0,1,2,3,6,14,7,8,9,4,5};
    for(unsigned i=0;i<sizeof(whgp_bit)/sizeof(whgp_bit[0]);++i)set_button(joystick,(int)i,(s->buttons&(1u<<whgp_bit[i]))!=0);
    uint8_t hat=0;if(s->hat_y>0)hat|=1;if(s->hat_x>0)hat|=2;if(s->hat_y<0)hat|=4;if(s->hat_x<0)hat|=8;
    set_hat(joystick,0,hat);
}
static int connect_hub(const char *path) {
    if(!path||path[0]!='/'||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return -1;}return fd;
}
static void *run(void *unused) {
    (void)unused;void *library=NULL,*joystick=NULL;uint32_t id=0;
    const char *root=getenv("STEAMOHOS_CODE_ROOT"),*hub=getenv("STEAMOHOS_GAMEPAD_SOCKET");
    char path[4096];if(!root||snprintf(path,sizeof(path),"%s/steamrtarm64/libSDL3.so.0",root)>=(int)sizeof(path))return NULL;
    unsigned char packet[sizeof(struct whgp_header)+sizeof(struct whgp_state_v2)];size_t used=0;
    while(!atomic_load(&stopping)) {
        if(!library) {
            library=dlopen(path,RTLD_NOW|RTLD_NOLOAD);
            if(library) {
#define FIND(field,name) field=dlsym(library,name)
                FIND(was_init,"SDL_WasInit");FIND(attach_joystick,"SDL_AttachVirtualJoystick");FIND(open_joystick,"SDL_OpenJoystick");
                FIND(close_joystick,"SDL_CloseJoystick");FIND(detach_joystick,"SDL_DetachVirtualJoystick");
                FIND(set_axis,"SDL_SetJoystickVirtualAxis");FIND(set_button,"SDL_SetJoystickVirtualButton");FIND(set_hat,"SDL_SetJoystickVirtualHat");
#undef FIND
                if(!was_init||!attach_joystick||!open_joystick||!close_joystick||!detach_joystick||!set_axis||!set_button||!set_hat)break;
            }
        }
        if(library&&(was_init(0)&0x200u)) {
            if(!joystick) {
                HmosVirtualJoystickDesc desc={.version=sizeof(desc),.type=1,.naxes=6,.nbuttons=11,.nhats=1,
                    .button_mask=(1u<<11)-1,.axis_mask=(1u<<6)-1,.name="HmosDeck Controller",.Rumble=rumble};
                id=attach_joystick(&desc);if(id)joystick=open_joystick(id);
                if(joystick){struct whgp_state_v2 neutral={0};update(joystick,&neutral);fprintf(stderr,"HMOSDECK_GAMEPAD: SDL3 virtual gamepad attached id=%u\n",id);}
                else if(id){detach_joystick(id);id=0;}
            }
            pthread_mutex_lock(&connection);
            if(endpoint<0)endpoint=connect_hub(hub);
            int fd=endpoint;pthread_mutex_unlock(&connection);
            if(joystick&&fd>=0) {
                struct pollfd ready={fd,POLLIN,0};int n=poll(&ready,1,100);
                if(n<0&&errno==EINTR)continue;
                if(n>0) {
                    ssize_t got=recv(fd,packet+used,sizeof(packet)-used,0);
                    if(got>0) {
                        used+=(size_t)got;
                        if(used>=sizeof(struct whgp_header)) {
                            struct whgp_header header;memcpy(&header,packet,sizeof(header));
                            if(header.magic!=WHGP_MAGIC||header.version!=WHGP_VERSION||header.msg_type!=WHGP_MSG_STATE||header.payload_size!=sizeof(struct whgp_state_v2))got=0;
                            else if(used==sizeof(packet)) {
                                struct whgp_state_v2 state;memcpy(&state,packet+sizeof(header),sizeof(state));
                                if(header.slot==0)update(joystick,&state);
                                used=0;
                            }
                        }
                    }
                    if(got<=0&&(got==0||(errno!=EAGAIN&&errno!=EINTR))) {
                        pthread_mutex_lock(&connection);close(endpoint);endpoint=-1;pthread_mutex_unlock(&connection);
                        used=0;struct whgp_state_v2 neutral={0};update(joystick,&neutral);
                    }
                }
                continue;
            }
        }
        struct timespec pause={0,100000000};nanosleep(&pause,NULL);
    }
    pthread_mutex_lock(&connection);if(endpoint>=0)close(endpoint);endpoint=-1;pthread_mutex_unlock(&connection);
    if(joystick&&was_init&&(was_init(0)&0x200u)){close_joystick(joystick);detach_joystick(id);}
    if(library)dlclose(library);
    return NULL;
}
__attribute__((constructor)) static void initialize(void) {
    const char *root=getenv("STEAMOHOS_CODE_ROOT"),*exec=getenv("STEAMOHOS_EXEC_PATH"),*hub=getenv("STEAMOHOS_GAMEPAD_SOCKET");
    if(!root||!exec||!hub)return;
    char expected[4096];if(snprintf(expected,sizeof(expected),"%s/steamrtarm64/steam",root)>=(int)sizeof(expected)||strcmp(exec,expected))return;
    running=pthread_create(&worker,NULL,run,NULL)==0;
}
__attribute__((destructor)) static void finalize(void) {
    if(running){atomic_store(&stopping,true);pthread_join(worker,NULL);}
}
