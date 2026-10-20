// A native GNU desktop-session supervisor. The client ELF stays unchanged.
// Read-only X11 diagnostics distinguish client startup from visible output.
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <dlfcn.h>
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
#include "pointer_observer.h"
#include "wine_window_bridge.h"

static uint64_t now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
#include "deck_session_control.h"
static int xerror(Display *d,XErrorEvent *e) {
    char text[128];XGetErrorText(d,e->error_code,text,sizeof(text));
    fprintf(stderr,"GNU_DESKTOP xerror code=%u request=%u resource=%lu text=%s\n",
        e->error_code,e->request_code,e->resourceid,text);return 0;
}
static unsigned windows(Display *d,Window window,unsigned depth,unsigned *budget) {
    Window root,parent,*children=NULL;unsigned count=0,mapped=0;
    if(!*budget||depth>4||!XQueryTree(d,window,&root,&parent,&children,&count))return 0;
    for(unsigned i=0;i<count&&*budget;++i) {
        --*budget;XWindowAttributes a;
        if(!XGetWindowAttributes(d,children[i],&a))continue;
        char *name=NULL;XFetchName(d,children[i],&name);
        if(depth==0||a.map_state==IsViewable||name) {
            char clean[161];size_t length=name?strnlen(name,160):0;
            for(size_t j=0;j<length;++j)clean[j]=(unsigned char)name[j]>=32?name[j]:'?';
            clean[length]=0;
            fprintf(stderr,"GNU_DESKTOP window=%lu depth=%u x=%d y=%d width=%d height=%d mapped=%d override=%d name=%s\n",
                children[i],depth,a.x,a.y,a.width,a.height,a.map_state,a.override_redirect,clean);
        }
        if(name)XFree(name);
        if(a.map_state==IsViewable)++mapped;
        mapped+=windows(d,children[i],depth+1,budget);
    }
    if(children)XFree(children);
    return mapped;
}
// A rootful Xwayland viewport has no external XWM. Constrain ordinary
// top-level windows, including CEF settings/install dialogs, to its real area.
// Do not intercept ConfigureRequest, override-redirect menus or keyboard focus.
static void fit_windows(Display *d,Window root) {
    XWindowAttributes area;if(!XGetWindowAttributes(d,root,&area))return;
    Window returned,parent,*children=NULL;unsigned count=0;
    if(!XQueryTree(d,root,&returned,&parent,&children,&count))return;
    for(unsigned i=0;i<count&&i<128;++i){XWindowAttributes a;
        if(!XGetWindowAttributes(d,children[i],&a)||a.map_state!=IsViewable||a.override_redirect||a.class==InputOnly)continue;
        int width=a.width>area.width?area.width:a.width,height=a.height>area.height?area.height:a.height;
        int x=a.x,y=a.y;if(x+width>area.width)x=area.width-width;if(y+height>area.height)y=area.height-height;
        if(x<0)x=0;
        if(y<0)y=0;
        if(x!=a.x||y!=a.y||width!=a.width||height!=a.height) {
            XMoveResizeWindow(d,children[i],x,y,(unsigned)width,(unsigned)height);
            fprintf(stderr,"GNU_DESKTOP fit_window x=%d y=%d width=%d height=%d\n",x,y,width,height);
        }
    }
    if(children)XFree(children);
    XFlush(d);
}
static unsigned component(unsigned long pixel,unsigned long mask) {
    if(!mask)return 0;
    unsigned shift=0;while(!(mask&1)){mask>>=1;++shift;}
    return (unsigned)((((pixel>>shift)&mask)*255+mask/2)/mask);
}
static void snapshot(Display *d,Window root) {
    const char *directory=getenv("STEAMOHOS_TMP_ROOT");char path[PATH_MAX];
    if(!directory||directory[0]!='/'||snprintf(path,sizeof(path),"%s/desktop-root.ppm",directory)>=(int)sizeof(path))return;
    XWindowAttributes a;if(!XGetWindowAttributes(d,root,&a)||a.width<1||a.height<1||a.width>4096||a.height>4096)return;
    XImage *image=XGetImage(d,root,0,0,a.width,a.height,AllPlanes,ZPixmap);if(!image)return;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
    if(f) {
        fprintf(f,"P6\n%d %d\n255\n",a.width,a.height);unsigned char row[4096*3];
        for(int y=0;y<a.height;++y) {
            for(int x=0;x<a.width;++x) {
                unsigned long p=XGetPixel(image,x,y);row[3*x]=component(p,image->red_mask);
                row[3*x+1]=component(p,image->green_mask);row[3*x+2]=component(p,image->blue_mask);
            }
            if(fwrite(row,3,a.width,f)!=(size_t)a.width)break;
        }
        int error=ferror(f);if(fclose(f))error=1;
        fprintf(stderr,"GNU_DESKTOP snapshot width=%d height=%d written=%d\n",a.width,a.height,!error);
    } else if(fd>=0)close(fd);
    XDestroyImage(image);
}
static void input_probe(Display *d) {
    Window focus;int revert;XGetInputFocus(d,&focus,&revert);
    if(focus==None||focus==PointerRoot){fputs("GNU_DESKTOP input_probe no focused client\n",stderr);return;}
    void *library=dlopen("libXtst.so.6",RTLD_NOW|RTLD_LOCAL);
    if(!library){fputs("GNU_DESKTOP input_probe XTEST unavailable\n",stderr);return;}
    int (*fake)(Display*,unsigned,int,unsigned long)=(int(*)(Display*,unsigned,int,unsigned long))dlsym(library,"XTestFakeKeyEvent");
    unsigned sent=0;
    // Diagnostic only: no password, Return, form submission, or clipboard use.
    const char text[]="hmosdeck";
    if(fake)for(size_t i=0;i<sizeof(text)-1;++i) {
        KeyCode code=XKeysymToKeycode(d,(KeySym)(unsigned char)text[i]);
        if(!code||!fake(d,code,True,0)||!fake(d,code,False,0))break;
        ++sent;
    }
    // Xlib retains extension callbacks registered by libXtst. Keep the module
    // loaded until this diagnostic process exits, after XCloseDisplay.
    XSync(d,False);
    fprintf(stderr,"GNU_DESKTOP input_probe focus=%lu diagnostic_keys_sent=%u\n",focus,sent);
}
int main(int argc,char **argv) {
    if(argc<2||argv[1][0]!='/'){fputs("GNU_DESKTOP invalid client\n",stderr);return 2;}
    Display *display=XOpenDisplay(NULL);if(!display){fputs("GNU_DESKTOP display unavailable\n",stderr);return 3;}
    XSetErrorHandler(xerror);Window root=DefaultRootWindow(display);
    struct wine_window_bridge bridge;
    int wine_window=bridge_initialize(&bridge,display,root);
    struct deck_control deck;int product=deck_initialize(&deck);
    pointer_observer_start(display,root);
    fprintf(stderr,"GNU_DESKTOP root width=%d height=%d\n",DisplayWidth(display,DefaultScreen(display)),DisplayHeight(display,DefaultScreen(display)));
    int (*prepare_cloud)(void)=dlsym(RTLD_DEFAULT,"SteamOhosPrepareCloudPrefixes");
    if(prepare_cloud&&prepare_cloud()<0)
        fputs("GNU_DESKTOP Cloud prefix preparation failed; existing saves unchanged\n",stderr);
    pid_t owner=getpid(),child=fork();
    if(child<0){XCloseDisplay(display);return 4;}
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=owner)_exit(126);
        close(ConnectionNumber(display));execv(argv[1],argv+1);perror("GNU_DESKTOP exec");_exit(127);
    }
    const char *requested=getenv("STEAMOHOS_DESKTOP_INPUT");
    int probe_requested=requested&&!strcmp(requested,"1");
    const char *bounds=getenv("STEAMOHOS_WINDOW_BOUNDS");
    int constrain=bounds&&!strcmp(bounds,"1");
    const char *observe=getenv("STEAMOHOS_DESKTOP_OBSERVE");
    int diagnostic=!product&&((!constrain&&!wine_window)||(observe&&!strcmp(observe,"1")));
    if(constrain) {
        unsigned long area[4]={0,0,(unsigned long)DisplayWidth(display,DefaultScreen(display)),(unsigned long)DisplayHeight(display,DefaultScreen(display))};
        XChangeProperty(display,root,XInternAtom(display,"_NET_WORKAREA",False),XA_CARDINAL,32,PropModeReplace,(unsigned char*)area,4);
        XFlush(display);
    }
    int status=0;uint64_t start=now(),next=start+12000,next_fit=start;
    for(;;) {
        pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child)break;
        if(result<0&&errno!=EINTR){XCloseDisplay(display);return 5;}
        if(wine_window)bridge_tick(&bridge);
        if(product)deck_tick(&deck,wine_window&&bridge.primary!=None,bridge.mode);
        if(constrain&&now()>=next_fit){fit_windows(display,root);next_fit=now()+250;}
        if(diagnostic&&now()>=next) {
            unsigned budget=128;unsigned mapped=windows(display,root,0,&budget);
            fprintf(stderr,"GNU_DESKTOP observation elapsed_ms=%llu mapped=%u\n",(unsigned long long)(now()-start),mapped);
            snapshot(display,root);
            if(probe_requested){input_probe(display);probe_requested=0;}
            next=now()+15000;
        }
        poll(NULL,0,50);
    }
    if(product)deck_finish(&deck);
    if(wine_window) {
        if(bridge.preferences_due) {bridge.preferences_due=1;bridge_save_preferences(&bridge);}
        bridge_publish(&bridge,1);
    }
    XCloseDisplay(display);
    if(WIFEXITED(status))return WEXITSTATUS(status);
    if(WIFSIGNALED(status)){signal(WTERMSIG(status),SIG_DFL);raise(WTERMSIG(status));}
    return 6;
}
