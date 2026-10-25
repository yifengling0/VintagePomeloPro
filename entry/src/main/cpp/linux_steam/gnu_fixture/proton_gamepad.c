/* GNU SDL2 virtual controller for the private Wine Proton experiment.
 * Compiled into the experimental SDL2, loaded only when Wine opens SDL.
 * Uses the existing ControllerHub protocol; no evdev or uinput emulation. */
#define _GNU_SOURCE
#include <SDL.h>
#include <stdbool.h>
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
static int (*attach_joystick)(const SDL_VirtualJoystickDesc*);
static void *(*open_joystick)(int);
static void (*close_joystick)(void*);
static int (*detach_joystick)(int);
static int (*set_axis)(void*,int,int16_t);
static int (*set_button)(void*,int,uint8_t);
static int (*set_hat)(void*,int,uint8_t);
static void (*lock_joysticks)(void);
static void (*unlock_joysticks)(void);
static int rumble(void *unused,uint16_t low,uint16_t high) {
    (void)unused;
    struct {struct whgp_header header;struct whgp_rumble_v1 body;} packet={
        {WHGP_MAGIC,WHGP_VERSION,WHGP_MSG_RUMBLE,0,sizeof(struct whgp_rumble_v1)},{low,high,0}};
    pthread_mutex_lock(&connection);
    bool sent=endpoint>=0&&send(endpoint,&packet,sizeof(packet),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(packet);
    pthread_mutex_unlock(&connection);return sent?0:-1;
}
static int16_t invert_y(int16_t y){return y==INT16_MIN?INT16_MAX:(int16_t)-y;}
static int16_t trigger(uint16_t value){if(value>32767)value=32767;return (int16_t)((uint32_t)value*65535/32767-32768);}
static void update(void *joystick,const struct whgp_state_v2 *s) {
    lock_joysticks();
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
    // SDL's virtual GameController mapping uses D-pad buttons, not the
    // legacy joystick hat. Publish both views of the same directional state.
    set_button(joystick,SDL_CONTROLLER_BUTTON_DPAD_UP,s->hat_y>0);
    set_button(joystick,SDL_CONTROLLER_BUTTON_DPAD_DOWN,s->hat_y<0);
    set_button(joystick,SDL_CONTROLLER_BUTTON_DPAD_LEFT,s->hat_x<0);
    set_button(joystick,SDL_CONTROLLER_BUTTON_DPAD_RIGHT,s->hat_x>0);
    unlock_joysticks();
}
static int connect_hub(const char *path) {
    if(!path||path[0]!='/'||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return -1;}return fd;
}
static void *run(void *unused) {
    (void)unused;void *library=NULL,*joystick=NULL;int id=-1;
    const char *root=getenv("STEAMOHOS_CODE_ROOT"),*hub=getenv("STEAMOHOS_GAMEPAD_SOCKET");
    char path[4096];if(!root||snprintf(path,sizeof(path),"%s/libSDL2-2.0.so.0",root)>=(int)sizeof(path))return NULL;
    unsigned char packet[sizeof(struct whgp_header)+sizeof(struct whgp_state_v2)];size_t used=0;
    while(!atomic_load(&stopping)) {
        if(!library) {
            library=dlopen(path,RTLD_NOW|RTLD_NOLOAD);
            if(library) {
#define FIND(field,name) field=dlsym(library,name)
                FIND(was_init,"SDL_WasInit");FIND(attach_joystick,"SDL_JoystickAttachVirtualEx");FIND(open_joystick,"SDL_JoystickOpen");
                FIND(close_joystick,"SDL_JoystickClose");FIND(detach_joystick,"SDL_JoystickDetachVirtual");
                FIND(set_axis,"SDL_JoystickSetVirtualAxis");FIND(set_button,"SDL_JoystickSetVirtualButton");FIND(set_hat,"SDL_JoystickSetVirtualHat");
                FIND(lock_joysticks,"SDL_LockJoysticks");FIND(unlock_joysticks,"SDL_UnlockJoysticks");
#undef FIND
                if(!was_init||!attach_joystick||!open_joystick||!close_joystick||!detach_joystick||!set_axis||!set_button||!set_hat||!lock_joysticks||!unlock_joysticks)break;
            }
        }
        if(library&&(was_init(0)&0x200u)) {
            if(!joystick) {
                SDL_VirtualJoystickDesc desc={.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION,.type=1,.naxes=6,.nbuttons=15,.nhats=1,
                    .button_mask=(1u<<15)-1,.axis_mask=(1u<<6)-1,.name="HmosDeck Controller",.Rumble=rumble};
                id=attach_joystick(&desc);if(id>=0)joystick=open_joystick(id);
                if(joystick){struct whgp_state_v2 neutral={0};update(joystick,&neutral);fprintf(stderr,"HMOSDECK_GAMEPAD: SDL2 virtual gamepad attached id=%d\n",id);}
                else if(id>=0){detach_joystick(id);id=-1;}
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
    char expected[4096];if(snprintf(expected,sizeof(expected),"%s/package/files/lib/wine/aarch64-unix/wine",root)>=(int)sizeof(expected)||strcmp(exec,expected)||strcmp(root,"/data/storage/el2/base/files/steamohos-native/proton-probe"))return;
    running=pthread_create(&worker,NULL,run,NULL)==0;
}
__attribute__((destructor)) static void finalize(void) {
    if(running){atomic_store(&stopping,true);pthread_join(worker,NULL);}
}
