/* Exercise persisted placement through real X11/ICCCM/EWMH requests. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void wait_mode(Window w,int mode) {
    char path[4096],text[1024],expected[96];
    snprintf(path,sizeof(path),"%s/wine-window-native.json",getenv("STEAMOHOS_TMP_ROOT"));
    snprintf(expected,sizeof(expected),"\"window\":%lu,\"mode\":%d,\"minimized\":0",w,mode);
    for(int n=0;n<150;++n) {
        FILE *f=fopen(path,"r");
        if(f) {size_t k=fread(text,1,sizeof(text)-1,f);fclose(f);text[k]=0;if(strstr(text,expected))return;}
        usleep(20000);
    }
    fprintf(stderr,"missing %s\n",expected);abort();
}
static void wait_preferences(int mode,int width,int height) {
    char path[4096];
    snprintf(path,sizeof(path),"%s/wine-window-preferences.conf",getenv("STEAMOHOS_TMP_ROOT"));
    for(int n=0;n<150;++n) {
        FILE *f=fopen(path,"r");int v,m,w,h,ok=0;
        if(f) {ok=fscanf(f,"%d %d %d %d",&v,&m,&w,&h)==4&&v==1&&m==mode&&w==width&&h==height;fclose(f);}
        if(ok)return;
        usleep(20000);
    }
    fputs("missing persisted desktop placement\n",stderr);abort();
}
static void request(Display *d,Window w,const char *property,int action) {
    XEvent e={0};e.xclient.type=ClientMessage;e.xclient.window=w;e.xclient.format=32;
    e.xclient.message_type=XInternAtom(d,"_NET_WM_STATE",False);e.xclient.data.l[0]=action;
    e.xclient.data.l[1]=(long)XInternAtom(d,property,False);
    XSendEvent(d,DefaultRootWindow(d),False,SubstructureRedirectMask|SubstructureNotifyMask,&e);XFlush(d);
}
int main(void) {
    Display *d=XOpenDisplay(NULL);assert(d);
    int restored=!strcmp(getenv("HMOSDECK_PLACEMENT_FIXTURE"),"restored");
    Window w=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,640,400,0,0,0);
    XStoreName(d,w,"Sign in to Steam");XMapWindow(d,w);XFlush(d);wait_mode(w,0);
    // A login window may be replaced or retitled when authentication completes.
    XStoreName(d,w,"Steam");XFlush(d);wait_mode(w,restored?0:1);
    if(restored) {
        wait_preferences(0,720,460);
        int match=0;
        for(int n=0;n<150;++n) {
            XWindowAttributes a;XGetWindowAttributes(d,w,&a);
            if(a.width==720&&a.height==460){match=1;break;}
            usleep(20000);
        }
        assert(match);
    } else {
        wait_preferences(1,640,400);
        request(d,w,"_NET_WM_STATE_MAXIMIZED_HORZ",2);wait_mode(w,0);
        XResizeWindow(d,w,720,460);XFlush(d);usleep(100000);
        wait_preferences(0,720,460);
    }
    request(d,w,"_NET_WM_STATE_FULLSCREEN",1);wait_mode(w,2);
    usleep(700000);wait_preferences(0,720,460);
    request(d,w,"_NET_WM_STATE_FULLSCREEN",0);wait_mode(w,0);
    wait_preferences(0,720,460);
    if(restored) {
        request(d,w,"_NET_WM_STATE_MAXIMIZED_HORZ",1);wait_mode(w,1);wait_preferences(1,720,460);
        request(d,w,"_NET_WM_STATE_FULLSCREEN",1);wait_mode(w,2);
        request(d,w,"_NET_WM_STATE_FULLSCREEN",0);wait_mode(w,1);
        wait_preferences(1,720,460);
    }
    XDestroyWindow(d,w);XCloseDisplay(d);
    puts("X11_PREFERENCES login isolation default maximize restore size fullscreen roundtrip PASS");
    return 37;
}
