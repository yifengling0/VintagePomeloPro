/* Drive the production supervisor via real X11 requests, not its C helpers. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void state(Window w,int mode,int minimized) {
    char path[4096],text[1024],expected[100];
    snprintf(path,sizeof(path),"%s/wine-window-native.json",getenv("STEAMOHOS_TMP_ROOT"));
    snprintf(expected,sizeof(expected),"\"window\":%lu,\"mode\":%d,\"minimized\":%d",w,mode,minimized);
    for(int n=0;n<100;++n) {
        FILE *file=fopen(path,"r");
        if(file) {
            size_t size=fread(text,1,sizeof(text)-1,file);fclose(file);text[size]=0;
            if(strstr(text,expected))return;
        }
        usleep(20000);
    }
    fprintf(stderr,"missing state %s\n",expected);abort();
}
static void request(Display *d,Window w,const char *type,long action,const char *first,const char *second) {
    XEvent e={0};e.xclient.type=ClientMessage;e.xclient.window=w;
    e.xclient.message_type=XInternAtom(d,type,False);e.xclient.format=32;
    e.xclient.data.l[0]=action;
    if(first)e.xclient.data.l[1]=(long)XInternAtom(d,first,False);
    if(second)e.xclient.data.l[2]=(long)XInternAtom(d,second,False);
    assert(XSendEvent(d,DefaultRootWindow(d),False,SubstructureRedirectMask|SubstructureNotifyMask,&e));
    XFlush(d);
}
static void bounds(Display *d,Window w,int width,int height) {
    for(int n=0;n<100;++n) {
        XWindowAttributes a;XGetWindowAttributes(d,w,&a);
        if(a.width==width&&a.height==height)return;
        usleep(20000);
    }
    fputs("missing window geometry\n",stderr);abort();
}
static unsigned moves(void) {
    char path[4096],content[1024];
    snprintf(path,sizeof(path),"%s/wine-window-native.json",getenv("STEAMOHOS_TMP_ROOT"));
    FILE *file=fopen(path,"r");assert(file);
    size_t bytes=fread(content,1,sizeof(content)-1,file);content[bytes]=0;fclose(file);
    char *value=strstr(content,"\"moveRequest\":");assert(value);
    return (unsigned)strtoul(value+14,NULL,10);
}
static void caption_pointer_tests(Display *d,Window w,Window popup) {
    void *xtst=dlopen("libXtst.so.6",RTLD_NOW);assert(xtst);
    int (*motion)(Display*,int,int,int,unsigned long)=dlsym(xtst,"XTestFakeMotionEvent");
    int (*button)(Display*,unsigned int,int,unsigned long)=dlsym(xtst,"XTestFakeButtonEvent");
    assert(motion&&button);
    XUnmapWindow(d,popup);XStoreName(d,w,"Steam");XResizeWindow(d,w,800,400);
    XRaiseWindow(d,w);XSync(d,False);usleep(150000);bounds(d,w,800,400);
    XWindowAttributes a;assert(XGetWindowAttributes(d,w,&a));
    unsigned before=moves();
    /* A normal click must remain a click; no moving on button-down. */
    motion(d,DefaultScreen(d),a.x+330,a.y+10,0);button(d,1,True,0);XSync(d,False);usleep(150000);
    assert(moves()==before);
    button(d,1,False,0);XSync(d,False);usleep(150000);assert(moves()==before);
    /* Dragging the known empty caption sends one request, with no repeated
     * requests while held. Wine's existing move grab supplies smooth motion. */
    button(d,1,True,0);XSync(d,False);usleep(150000);
    motion(d,DefaultScreen(d),a.x+350,a.y+15,0);XSync(d,False);usleep(150000);assert(moves()==before+1);
    motion(d,DefaultScreen(d),a.x+370,a.y+20,0);XSync(d,False);usleep(150000);assert(moves()==before+1);
    button(d,1,False,0);XSync(d,False);usleep(150000);
    /* Menu area, non-Steam windows and fullscreen never use the fallback. */
    for(int region=0;region<3;++region) {
        if(region==1)XStoreName(d,w,"Other application");
        if(region==2) {
            XStoreName(d,w,"Steam");
            request(d,w,"_NET_WM_STATE",1,"_NET_WM_STATE_FULLSCREEN",NULL);state(w,2,0);
        }
        int x=region==0?200:330;
        assert(XGetWindowAttributes(d,w,&a));
        motion(d,DefaultScreen(d),a.x+x,a.y+10,0);button(d,1,True,0);XSync(d,False);usleep(150000);
        motion(d,DefaultScreen(d),a.x+x+20,a.y+15,0);XSync(d,False);usleep(150000);
        button(d,1,False,0);XSync(d,False);usleep(150000);assert(moves()==before+1);
    }
    request(d,w,"_NET_WM_STATE",0,"_NET_WM_STATE_FULLSCREEN",NULL);state(w,0,0);
    /* Xlib retains extension callbacks until XCloseDisplay; keep Xtst loaded
     * for the lifetime of this short fixture process. */
    puts("X11_CAPTION click drag once menus identity fullscreen PASS");
}
int main(void) {
    Display *d=XOpenDisplay(NULL);assert(d);
    Window root=DefaultRootWindow(d);
    Window w=XCreateSimpleWindow(d,root,20,20,640,400,0,0,0x123456);
    XMapWindow(d,w);XFlush(d);state(w,0,0);
    /* A larger transient dialog must not steal primary-window ownership. */
    Window popup=XCreateSimpleWindow(d,root,10,10,760,560,0,0,0);
    XSetTransientForHint(d,popup,w);XMapWindow(d,popup);XFlush(d);usleep(150000);state(w,0,0);
    request(d,w,"_NET_WM_STATE",1,"_NET_WM_STATE_FULLSCREEN",NULL);state(w,2,0);bounds(d,w,800,600);
    request(d,w,"_NET_WM_STATE",0,"_NET_WM_STATE_MAXIMIZED_HORZ","_NET_WM_STATE_MAXIMIZED_VERT");
    usleep(150000);state(w,2,0);
    request(d,w,"_NET_WM_STATE",2,"_NET_WM_STATE_FULLSCREEN",NULL);state(w,0,0);bounds(d,w,640,400);
    request(d,w,"_NET_WM_STATE",2,"_NET_WM_STATE_MAXIMIZED_HORZ","_NET_WM_STATE_MAXIMIZED_VERT");
    state(w,1,0);bounds(d,w,800,600);
    request(d,w,"_NET_WM_STATE",2,"_NET_WM_STATE_MAXIMIZED_HORZ","_NET_WM_STATE_MAXIMIZED_VERT");
    state(w,0,0);bounds(d,w,640,400);
    XEvent move={0};move.xclient.type=ClientMessage;move.xclient.window=w;move.xclient.format=32;
    move.xclient.message_type=XInternAtom(d,"_NET_WM_MOVERESIZE",False);
    move.xclient.data.l[2]=8;move.xclient.data.l[3]=1;
    XSendEvent(d,root,False,SubstructureRedirectMask|SubstructureNotifyMask,&move);XFlush(d);
    usleep(150000);
    char path[4096],content[1024];snprintf(path,sizeof(path),"%s/wine-window-native.json",getenv("STEAMOHOS_TMP_ROOT"));
    FILE *file=fopen(path,"r");assert(file);
    size_t bytes=fread(content,1,sizeof(content)-1,file);content[bytes]=0;fclose(file);
    assert(strstr(content,"\"moveRequest\":1"));
    caption_pointer_tests(d,w,popup);
    request(d,w,"WM_CHANGE_STATE",IconicState,NULL,NULL);state(w,0,1);
    /* Restore from Wine taskbar is a command to remap the same X11 window. */
    snprintf(path,sizeof(path),"%s/wine-window-command.txt",getenv("STEAMOHOS_TMP_ROOT"));
    file=fopen(path,"w");assert(file);fputs("1 1\n",file);fclose(file);state(w,0,0);
    XWindowAttributes a;XGetWindowAttributes(d,w,&a);assert(a.map_state==IsViewable);
    Window focus;int revert;XGetInputFocus(d,&focus,&revert);assert(focus==w);
    XDestroyWindow(d,popup);XDestroyWindow(d,w);XFlush(d);state(None,0,0);
    Window replacement=XCreateSimpleWindow(d,root,0,0,640,400,0,0,0);
    XMapWindow(d,replacement);XFlush(d);state(replacement,0,0);
    // A mode switch withdraws the old primary without destroying it.
    // Transient dialogs must not replace it; a new normal main window must.
    request(d,replacement,"_NET_WM_STATE",1,"_NET_WM_STATE_FULLSCREEN",NULL);state(replacement,2,0);
    XUnmapWindow(d,replacement);XFlush(d);state(None,0,0);
    Window desktop=XCreateSimpleWindow(d,root,0,0,640,400,0,0,0);
    XMapWindow(d,desktop);XFlush(d);state(desktop,0,0);
    request(d,desktop,"WM_CHANGE_STATE",IconicState,NULL,NULL);state(desktop,0,1);
    XMapRaised(d,desktop);XFlush(d);state(desktop,0,0);
    XUnmapWindow(d,desktop);XMapRaised(d,replacement);XFlush(d);state(replacement,2,0);
    XDestroyWindow(d,desktop);XDestroyWindow(d,replacement);XCloseDisplay(d);
    puts("X11_MODE_SWITCH withdrawal replacement remap retained-fullscreen PASS");
    puts("X11_BRIDGE primary popup fullscreen maximize minimize restore generation PASS");
    return 37;
}
