/* Opt-in, bounded pointer-only diagnostics for the private X11 display.
 * No key events, text, window names or pixels are collected. */
#include <X11/extensions/XInput2.h>
static FILE *pointer_log;
static int pointer_opcode;
static unsigned pointer_lines;
static void pointer_observer_start(Display *d,Window root) {
    const char *temporary=getenv("STEAMOHOS_TMP_ROOT");char path[PATH_MAX];
    if(!temporary||snprintf(path,sizeof(path),"%s/input-observe.enable",temporary)>=(int)sizeof(path)||access(path,F_OK))return;
    void *xi=dlopen("libXi.so.6",RTLD_NOW|RTLD_LOCAL);
    int (*select_events)(Display*,Window,XIEventMask*,int)=xi?dlsym(xi,"XISelectEvents"):NULL;
    int event,error;if(!select_events||!XQueryExtension(d,"XInputExtension",&pointer_opcode,&event,&error))return;
    if(snprintf(path,sizeof(path),"%s/input-observe.txt",temporary)>=(int)sizeof(path))return;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return;
    pointer_log=fdopen(fd,"w");if(!pointer_log){close(fd);return;}
    unsigned char mask[XIMaskLen(XI_LASTEVENT)]={0};
    XISetMask(mask,XI_RawMotion);XISetMask(mask,XI_RawButtonPress);XISetMask(mask,XI_RawButtonRelease);
    XIEventMask events={XIAllMasterDevices,sizeof(mask),mask};
    select_events(d,root,&events,1);XFlush(d);
    fputs("pointer_only=1 max_records=1024\n",pointer_log);fflush(pointer_log);
    // Xlib keeps extension hooks; libXi must remain loaded until XCloseDisplay.
}
static void pointer_observer_event(Display *d,XEvent *e) {
    if(!pointer_log||pointer_lines>=1024||e->type!=GenericEvent||e->xcookie.extension!=pointer_opcode||!XGetEventData(d,&e->xcookie))return;
    if(e->xcookie.evtype==XI_RawMotion) {
        XIRawEvent *raw=e->xcookie.data;double *value=raw->raw_values;
        for(int i=0;i<raw->valuators.mask_len*8;++i)if(XIMaskIsSet(raw->valuators.mask,i)) {
            if(i>1&&*value!=0&&pointer_lines++<1024)
                fprintf(pointer_log,"time=%lu event=scroll source=%d valuator=%d delta=%.4f\n",raw->time,raw->sourceid,i,*value);
            ++value;
        }
    } else if(e->xcookie.evtype==XI_RawButtonPress||e->xcookie.evtype==XI_RawButtonRelease) {
        XIRawEvent *raw=e->xcookie.data;
        if(pointer_lines++<1024)fprintf(pointer_log,"time=%lu event=button detail=%d down=%d\n",raw->time,raw->detail,e->xcookie.evtype==XI_RawButtonPress);
    }
    fflush(pointer_log);XFreeEventData(d,&e->xcookie);
}
