/* Minimal X11 state manager for the private rootful Steam display. Wine owns
 * the visible HWND; Steam owns its client decorations. No image capture. */
struct wine_window_bridge {
    Display *display; Window root, primary, support;
    Atom state, fullscreen, max_h, max_v, change_state, active, wm_state, moveresize;
    int mode, minimized; unsigned activation, serial, command, move_request;
    int normal_x, normal_y, normal_width, normal_height;
    char last_type[64]; Window last_target; long last_direction,last_button;
    int caption_pressed,caption_pending,caption_x,caption_y;
    int preferences_enabled,desktop_mode,saved_width,saved_height;
    Window preferences_window;
    char preferences[PATH_MAX],previous_preferences[96];
    uint64_t preferences_due;
    char path[PATH_MAX], commands[PATH_MAX], previous[512];
};
static int bridge_property(Display *d, Window w, Atom property, Atom expected,
                           unsigned long **values, unsigned long *count) {
    Atom type; int format; unsigned long remaining; unsigned char *data=NULL;
    if(XGetWindowProperty(d,w,property,0,64,False,expected,&type,&format,count,&remaining,&data)!=Success)
        return 0;
    if(type!=expected||format!=32||remaining){if(data)XFree(data);return 0;}
    *values=(unsigned long*)data; return 1;
}
static int bridge_has(struct wine_window_bridge *b,Window w,Atom wanted) {
    unsigned long *values=NULL,count=0;int found=0;
    if(bridge_property(b->display,w,b->state,XA_ATOM,&values,&count)) {
        for(unsigned long i=0;i<count;++i)if(values[i]==wanted)found=1;
        if(values)XFree(values);
    }
    return found;
}
static int bridge_is_desktop(struct wine_window_bridge *b) {
    char *name=NULL;int desktop=0;
    if(b->primary&&XFetchName(b->display,b->primary,&name)&&name)desktop=!strcmp(name,"Steam");
    if(name)XFree(name);
    return desktop;
}
static uint64_t bridge_clock(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void bridge_save_preferences(struct wine_window_bridge *b) {
    if(!b->preferences_enabled||!b->preferences_due||bridge_clock()<b->preferences_due)return;
    b->preferences_due=0;
    char text[96],temporary[PATH_MAX];
    snprintf(text,sizeof(text),"1 %d %d %d\n",b->desktop_mode,b->saved_width,b->saved_height);
    if(!strcmp(text,b->previous_preferences)||
       snprintf(temporary,sizeof(temporary),"%s.tmp",b->preferences)>=(int)sizeof(temporary))return;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return;
    ssize_t size=write(fd,text,strlen(text));close(fd);
    if(size==(ssize_t)strlen(text)&&!rename(temporary,b->preferences))strcpy(b->previous_preferences,text);
}
static void bridge_remember_preferences(struct wine_window_bridge *b) {
    if(!b->preferences_enabled)return;
    if(b->normal_width>=320&&b->normal_width<=4096&&b->normal_height>=200&&b->normal_height<=4096&&
       (long)b->normal_width*b->normal_height<=8388608) {
        b->saved_width=b->normal_width;b->saved_height=b->normal_height;
    }
    b->preferences_due=bridge_clock()+600;
}
static void bridge_publish(struct wine_window_bridge *b,int closed) {
    char text[512],temporary[PATH_MAX];
    snprintf(text,sizeof(text),"{\"version\":1,\"window\":%lu,\"mode\":%d,\"minimized\":%d,\"activation\":%u,\"closed\":%d,\"normalWidth\":%d,\"normalHeight\":%d,\"moveRequest\":%u,\"request\":\"%s\",\"requestWindow\":%lu,\"direction\":%ld,\"button\":%ld}\n",
        b->primary,b->mode,b->minimized,b->activation,closed,b->normal_width,b->normal_height,b->move_request,
        b->last_type,b->last_target,b->last_direction,b->last_button);
    if(!strcmp(text,b->previous))return;
    if(snprintf(temporary,sizeof(temporary),"%s.tmp",b->path)>=(int)sizeof(temporary))return;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return;
    ssize_t size=write(fd,text,strlen(text));close(fd);
    if(size==(ssize_t)strlen(text)&&!rename(temporary,b->path))strcpy(b->previous,text);
}
static void bridge_set_state(struct wine_window_bridge *b) {
    Atom values[3];int count=0;
    if(b->mode==2)values[count++]=b->fullscreen;
    if(b->mode==1){values[count++]=b->max_h;values[count++]=b->max_v;}
    XChangeProperty(b->display,b->primary,b->state,XA_ATOM,32,PropModeReplace,(unsigned char*)values,count);
    unsigned long legacy[2]={b->minimized?IconicState:NormalState,None};
    XChangeProperty(b->display,b->primary,b->wm_state,b->wm_state,32,PropModeReplace,(unsigned char*)legacy,2);
}
static void bridge_layout(struct wine_window_bridge *b) {
    XWindowAttributes root,a;
    if(!b->primary||!XGetWindowAttributes(b->display,b->root,&root)||
       !XGetWindowAttributes(b->display,b->primary,&a))return;
    int x=0,y=0,width=root.width,height=root.height;
    if(b->mode==0) {
        width=b->normal_width>0?b->normal_width:a.width;
        height=b->normal_height>0?b->normal_height:a.height;
        if(width>root.width)width=root.width;
        if(height>root.height)height=root.height;
        x=b->normal_x;y=b->normal_y;
        if(x+width>root.width)x=root.width-width;
        if(y+height>root.height)y=root.height-height;
        if(x<0)x=0;
        if(y<0)y=0;
    }
    if(a.x!=x||a.y!=y||a.width!=width||a.height!=height)
        XMoveResizeWindow(b->display,b->primary,x,y,(unsigned)width,(unsigned)height);
}
/* ICCCM: also answer unchanged or constrained ConfigureRequests. */
static void bridge_configured(struct wine_window_bridge *b,Window window) {
    XWindowAttributes a;if(!XGetWindowAttributes(b->display,window,&a))return;
    XEvent event={0};event.xconfigure.type=ConfigureNotify;
    event.xconfigure.display=b->display;event.xconfigure.event=window;event.xconfigure.window=window;
    event.xconfigure.x=a.x;event.xconfigure.y=a.y;event.xconfigure.width=a.width;event.xconfigure.height=a.height;
    event.xconfigure.border_width=a.border_width;event.xconfigure.above=None;event.xconfigure.override_redirect=False;
    XSendEvent(b->display,window,False,StructureNotifyMask,&event);
}
static void bridge_mode(struct wine_window_bridge *b,int mode) {
    if(mode==b->mode)return;
    if(b->mode==0) {
        XWindowAttributes a;
        if(XGetWindowAttributes(b->display,b->primary,&a)) {
            b->normal_x=a.x;b->normal_y=a.y;b->normal_width=a.width;b->normal_height=a.height;
        }
    }
    b->mode=mode;bridge_layout(b);bridge_set_state(b);
}
static void bridge_apply_preferences(struct wine_window_bridge *b) {
    // Login, splash, transient dialogs and Big Picture do not own desktop size.
    if(!b->preferences_enabled||b->preferences_window==b->primary||b->mode==2||
       !bridge_is_desktop(b))return;
    b->preferences_window=b->primary;
    if(b->saved_width>0&&b->saved_height>0) {
        b->normal_width=b->saved_width;b->normal_height=b->saved_height;
    }
    b->mode=b->desktop_mode;bridge_layout(b);bridge_set_state(b);
    bridge_remember_preferences(b);
}
static void bridge_choose(struct wine_window_bridge *b) {
    Window returned,parent,*children=NULL;unsigned count=0;Window candidate=None;long largest=0;int best=-1;
    if(!XQueryTree(b->display,b->root,&returned,&parent,&children,&count))return;
    for(unsigned i=0;i<count&&i<256;++i) {
        XWindowAttributes a;Window transient=None;
        if(!XGetWindowAttributes(b->display,children[i],&a)||a.override_redirect||
            a.class==InputOnly||a.map_state!=IsViewable||a.width<300||a.height<200||
            XGetTransientForHint(b->display,children[i],&transient))continue;
        long area=(long)a.width*a.height;
        int rank=bridge_has(b,children[i],b->fullscreen)?1:0;
        // Prefer fullscreen; equal-sized replacements follow X11 stacking.
        if(rank>best||(rank==best&&area>=largest)){best=rank;largest=area;candidate=children[i];}
    }
    if(children)XFree(children);
    if(candidate&&candidate!=b->primary) {
        b->primary=candidate;b->minimized=0;
        XWindowAttributes a;
        if(XGetWindowAttributes(b->display,candidate,&a)) {
            b->normal_x=a.x;b->normal_y=a.y;b->normal_width=a.width;b->normal_height=a.height;
        }
        b->mode=bridge_has(b,candidate,b->fullscreen)?2:
            (bridge_has(b,candidate,b->max_h)&&bridge_has(b,candidate,b->max_v)?1:0);
        XSelectInput(b->display,candidate,PropertyChangeMask|StructureNotifyMask);
        pointer_observer_watch(b->display,candidate);
        const char *names[]={"_NET_WM_ACTION_MOVE","_NET_WM_ACTION_MINIMIZE","_NET_WM_ACTION_MAXIMIZE_HORZ",
            "_NET_WM_ACTION_MAXIMIZE_VERT","_NET_WM_ACTION_FULLSCREEN","_NET_WM_ACTION_CLOSE"};
        Atom actions[6];for(unsigned i=0;i<6;++i)actions[i]=XInternAtom(b->display,names[i],False);
        XChangeProperty(b->display,candidate,XInternAtom(b->display,"_NET_WM_ALLOWED_ACTIONS",False),
            XA_ATOM,32,PropModeReplace,(unsigned char*)actions,6);
        XSetInputFocus(b->display,candidate,RevertToParent,CurrentTime);
        ++b->activation;bridge_layout(b);bridge_set_state(b);
    }
}
static int bridge_initialize(struct wine_window_bridge *b,Display *d,Window root) {
    const char *enabled=getenv("STEAMOHOS_WINE_WINDOW_BRIDGE"),*directory=getenv("STEAMOHOS_TMP_ROOT");
    if(!enabled||strcmp(enabled,"1")||!directory||directory[0]!='/')return 0;
    memset(b,0,sizeof(*b));b->display=d;b->root=root;
    b->preferences_enabled=getenv("HMOSDECK_SESSION_GENERATION")!=NULL;
    b->desktop_mode=b->preferences_enabled?1:0;
    if(b->preferences_enabled&&snprintf(b->preferences,sizeof(b->preferences),"%s/wine-window-preferences.conf",directory)<(int)sizeof(b->preferences)) {
        int fd=open(b->preferences,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0) {
            char text[97]={0},extra;struct stat st;int version,mode,width,height;
            ssize_t count=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size>96?-1:read(fd,text,96);close(fd);
            if(count>0&&sscanf(text,"%d %d %d %d %c",&version,&mode,&width,&height,&extra)==4&&version==1&&
               (mode==0||mode==1)&&width>=320&&width<=4096&&height>=200&&height<=4096&&
               (long)width*height<=8388608) {
                b->desktop_mode=mode;b->saved_width=width;b->saved_height=height;
            }
        }
    } else b->preferences_enabled=0;
    if(snprintf(b->path,sizeof(b->path),"%s/wine-window-native.json",directory)>=(int)sizeof(b->path)||
       snprintf(b->commands,sizeof(b->commands),"%s/wine-window-command.txt",directory)>=(int)sizeof(b->commands))return 0;
    b->state=XInternAtom(d,"_NET_WM_STATE",False);b->fullscreen=XInternAtom(d,"_NET_WM_STATE_FULLSCREEN",False);
    b->max_h=XInternAtom(d,"_NET_WM_STATE_MAXIMIZED_HORZ",False);b->max_v=XInternAtom(d,"_NET_WM_STATE_MAXIMIZED_VERT",False);
    b->change_state=XInternAtom(d,"WM_CHANGE_STATE",False);b->active=XInternAtom(d,"_NET_ACTIVE_WINDOW",False);
    b->wm_state=XInternAtom(d,"WM_STATE",False);
    b->moveresize=XInternAtom(d,"_NET_WM_MOVERESIZE",False);
    XSelectInput(d,root,SubstructureRedirectMask|SubstructureNotifyMask|StructureNotifyMask);
    b->support=XCreateSimpleWindow(d,root,-1,-1,1,1,0,0,0);
    char selection[32];snprintf(selection,sizeof(selection),"WM_S%d",DefaultScreen(d));
    XSetSelectionOwner(d,XInternAtom(d,selection,False),b->support,CurrentTime);
    Atom check=XInternAtom(d,"_NET_SUPPORTING_WM_CHECK",False);
    unsigned long support=b->support;
    XChangeProperty(d,root,check,XA_WINDOW,32,PropModeReplace,(unsigned char*)&support,1);
    XChangeProperty(d,b->support,check,XA_WINDOW,32,PropModeReplace,(unsigned char*)&support,1);
    const char name[]="HmosDeck Wine window bridge";
    XChangeProperty(d,b->support,XInternAtom(d,"_NET_WM_NAME",False),XInternAtom(d,"UTF8_STRING",False),
        8,PropModeReplace,(const unsigned char*)name,sizeof(name)-1);
    Atom supported[]={b->state,b->fullscreen,b->max_h,b->max_v,b->active,b->moveresize,
        XInternAtom(d,"_NET_WM_ALLOWED_ACTIONS",False),XInternAtom(d,"_NET_WM_ACTION_MOVE",False)};
    XChangeProperty(d,root,XInternAtom(d,"_NET_SUPPORTED",False),XA_ATOM,32,PropModeReplace,(unsigned char*)supported,8);
    bridge_publish(b,0);XFlush(d);return 1;
}
static void bridge_caption_drag(struct wine_window_bridge *b) {
    /* The ARM64 Frame client can paint the desktop title without emitting an
     * EWMH move. Only its known empty central caption region gets a fallback;
     * menus, account/buttons, dialogs, gamepad UI and fullscreen are excluded.
     * Observe pointer state, not pixels. The existing Wine grab does all motion. */
    if(!b->primary||b->mode||b->minimized) {b->caption_pending=0;return;}
    const char *disabled=getenv("STEAMOHOS_DISABLE_CAPTION_DRAG");
    if(disabled&&!strcmp(disabled,"1"))return;
    Window root,child;int rx,ry,wx,wy;unsigned buttons;
    if(!XQueryPointer(b->display,b->root,&root,&child,&rx,&ry,&wx,&wy,&buttons))return;
    int pressed=!!(buttons&Button1Mask);
    if(pressed&&!b->caption_pressed) {
        char *name=NULL;XWindowAttributes a;
        if(child==b->primary&&XFetchName(b->display,b->primary,&name)&&name&&!strcmp(name,"Steam")&&
           XGetWindowAttributes(b->display,b->primary,&a)&&a.width>=800&&
           rx>=a.x+300&&rx<a.x+a.width-360&&ry>=a.y&&ry<a.y+32) {
            b->caption_pending=1;b->caption_x=rx;b->caption_y=ry;
        }
        if(name)XFree(name);
    }
    if(pressed&&b->caption_pending&&
       (abs(rx-b->caption_x)>=4||abs(ry-b->caption_y)>=4)) {
        ++b->move_request;b->caption_pending=0;
        strcpy(b->last_type,"CAPTION_DRAG_FALLBACK");b->last_target=b->primary;
    }
    if(!pressed)b->caption_pending=0;
    b->caption_pressed=pressed;
}
static void bridge_tick(struct wine_window_bridge *b) {
    Display *d=b->display;
    while(XPending(d)) {
        XEvent e;XNextEvent(d,&e);
        pointer_observer_event(d,&e);
        if(e.type==ClientMessage) {
            char *name=XGetAtomName(d,e.xclient.message_type);
            b->last_type[0]=0;
            if(name&&(!strncmp(name,"_NET_",5)||!strncmp(name,"WM_",3))) {
                unsigned i=0;
                while(name[i]&&i<sizeof(b->last_type)-1) {
                    char c=name[i];if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))break;
                    b->last_type[i++]=c;
                }
                b->last_type[i]=0;
            }
            if(name)XFree(name);
            b->last_target=e.xclient.window;b->last_direction=e.xclient.data.l[2];b->last_button=e.xclient.data.l[3];
        }
        if(e.type==MapRequest) {
            XMapWindow(d,e.xmaprequest.window);
        } else if(e.type==ConfigureRequest) {
            XConfigureRequestEvent *r=&e.xconfigurerequest;
            strcpy(b->last_type,"CONFIGURE_REQUEST");b->last_target=r->window;
            b->last_direction=(long)r->value_mask;b->last_button=r->x;
            XWindowChanges c={.x=r->x,.y=r->y,.width=r->width,.height=r->height,
                .border_width=r->border_width,.sibling=r->above,.stack_mode=r->detail};
            unsigned mask=(unsigned)r->value_mask;
            if(r->window==b->primary&&b->mode==0&&(mask&(CWX|CWY))) {
                Window root,child;int rx,ry,wx,wy;unsigned buttons;
                if(XQueryPointer(d,b->root,&root,&child,&rx,&ry,&wx,&wy,&buttons)&&(buttons&Button1Mask)) {
                    // Older clients move themselves instead of sending EWMH.
                    // Keep content at the X11 origin; move its Wine host.
                    ++b->move_request;mask&=~(CWX|CWY);
                }
            }
            if(r->window==b->primary&&b->mode==0) {
                XWindowAttributes a,root;
                if(XGetWindowAttributes(d,b->primary,&a)&&XGetWindowAttributes(d,b->root,&root)) {
                    int x=mask&CWX?c.x:a.x,y=mask&CWY?c.y:a.y;
                    int w=mask&CWWidth?c.width:a.width,h=mask&CWHeight?c.height:a.height;
                    if(w<1)w=a.width;
                    if(h<1)h=a.height;
                    if(w>root.width)w=root.width;
                    if(h>root.height)h=root.height;
                    if(x+w>root.width)x=root.width-w;
                    if(y+h>root.height)y=root.height-h;
                    if(x<0)x=0;
                    if(y<0)y=0;
                    c.x=b->normal_x=x;c.y=b->normal_y=y;
                    c.width=b->normal_width=w;c.height=b->normal_height=h;
                    if(b->preferences_window==b->primary&&bridge_is_desktop(b))bridge_remember_preferences(b);
                    mask|=CWX|CWY|CWWidth|CWHeight;
                }
            }
            if(mask)XConfigureWindow(d,r->window,mask,&c);
            if(r->window==b->primary) {
                if(b->mode!=0)bridge_layout(b);
                bridge_configured(b,r->window);
            }
        } else if(e.type==ConfigureNotify&&e.xconfigure.window==b->root) {
            bridge_layout(b);
        } else if(e.type==ClientMessage&&e.xclient.window==b->primary) {
            if(e.xclient.message_type==b->moveresize&&e.xclient.data.l[2]==8&&
               (e.xclient.data.l[3]==0||e.xclient.data.l[3]==1)&&b->mode==0&&!b->minimized) {
                ++b->move_request;
            } else if(e.xclient.message_type==b->change_state&&e.xclient.data.l[0]==IconicState) {
                b->minimized=1;XUnmapWindow(d,b->primary);bridge_set_state(b);
            } else if(e.xclient.message_type==b->state) {
                int action=(int)e.xclient.data.l[0];
                if(action<0||action>2)continue;
                for(int i=1;i<=2;++i) {
                    Atom a=(Atom)e.xclient.data.l[i];
                    int mode=a==b->fullscreen?2:(a==b->max_h||a==b->max_v?1:0);
                    int previous=b->mode;
                    if(mode)bridge_mode(b,action==0?(b->mode==mode?0:b->mode):
                        (action==1?mode:(b->mode==mode?0:mode)));
                    if(mode==2&&previous==2&&b->mode==0&&b->preferences_enabled&&b->preferences_window==b->primary)
                        bridge_mode(b,b->desktop_mode);
                    if(mode==1&&previous!=b->mode&&b->preferences_window==b->primary&&bridge_is_desktop(b)) {
                        b->desktop_mode=b->mode==1?1:0;bridge_remember_preferences(b);
                    }
                    // A paired horizontal/vertical request is one maximize toggle.
                    if(mode==1)break;
                }
                bridge_set_state(b);
            } else if(e.xclient.message_type==b->active) {
                b->minimized=0;XMapRaised(d,b->primary);
                XSetInputFocus(d,b->primary,RevertToParent,CurrentTime);++b->activation;
                bridge_set_state(b);
            }
        } else if(e.type==MapNotify&&e.xmap.window==b->primary) {
            if(b->minimized) {
                b->minimized=0;++b->activation;bridge_set_state(b);
            }
        } else if(e.type==UnmapNotify&&e.xunmap.window==b->primary) {
            // ICCCM iconify is WM_CHANGE_STATE above. Steam also withdraws its
            // Big Picture window to reveal/create the desktop UI. Treating
            // every unmap as iconify pins the hidden window and loses the UI.
            if(!b->minimized) {
                b->primary=None;b->mode=0;b->caption_pending=0;b->caption_pressed=0;
            }
        } else if(e.type==DestroyNotify&&e.xdestroywindow.window==b->primary) {
            b->primary=None;b->minimized=0;b->mode=0;
            b->caption_pending=0;b->caption_pressed=0;
        }
    }
    // Keep a hidden primary stable: a temporary popup must never replace it.
    if(!b->primary||!b->minimized)bridge_choose(b);
    int fd=open(b->commands,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        char command[80]={0};ssize_t size=read(fd,command,sizeof(command)-1);close(fd);
        unsigned serial;int action;
        if(size>0&&sscanf(command,"%u %d",&serial,&action)==2&&serial!=b->command) {
            b->command=serial;
            if(action==1&&b->primary) {
                b->minimized=0;XMapRaised(d,b->primary);
                XSetInputFocus(d,b->primary,RevertToParent,CurrentTime);bridge_set_state(b);
            }
        }
    }
    bridge_apply_preferences(b);bridge_save_preferences(b);
    bridge_caption_drag(b);bridge_publish(b,0);XFlush(d);
}
