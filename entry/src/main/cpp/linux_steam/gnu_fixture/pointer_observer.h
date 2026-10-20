/* Opt-in, bounded pointer-only diagnostics for the private X11 display.
 * No key events, text, window names or pixels are collected. */
#include <X11/extensions/XInput2.h>
static FILE *pointer_log;
static int pointer_opcode;
static unsigned pointer_lines;
static XIDeviceInfo *(*pointer_query_device)(Display*,int,int*);
static void (*pointer_free_device)(XIDeviceInfo*);
static void pointer_observer_start(Display *d,Window root) {
    const char *temporary=getenv("STEAMOHOS_TMP_ROOT");char path[PATH_MAX];
    if(!temporary||snprintf(path,sizeof(path),"%s/input-observe.enable",temporary)>=(int)sizeof(path)||access(path,F_OK))return;
    void *xi=dlopen("libXi.so.6",RTLD_NOW|RTLD_LOCAL);
    int (*select_events)(Display*,Window,XIEventMask*,int)=xi?dlsym(xi,"XISelectEvents"):NULL;
    pointer_query_device=xi?dlsym(xi,"XIQueryDevice"):NULL;
    pointer_free_device=xi?dlsym(xi,"XIFreeDeviceInfo"):NULL;
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
static void pointer_observer_watch(Display *d,Window w) {
    if(!pointer_log)return;
    void *xi=dlopen("libXi.so.6",RTLD_NOW|RTLD_LOCAL);
    int (*select_events)(Display*,Window,XIEventMask*,int)=xi?dlsym(xi,"XISelectEvents"):NULL;
    if(!select_events)return;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)]={0};
    XISetMask(mask,XI_Motion);XISetMask(mask,XI_Enter);XISetMask(mask,XI_Leave);
    XIEventMask events={XIAllMasterDevices,sizeof(mask),mask};select_events(d,w,&events,1);
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
        if(pointer_query_device&&pointer_free_device&&pointer_lines<1024) {
            int count=0;XIDeviceInfo *devices=pointer_query_device(d,raw->sourceid,&count);
            for(int n=0;devices&&n<count;++n)for(int c=0;c<devices[n].num_classes;++c) {
                XIAnyClassInfo *item=devices[n].classes[c];
                if(item->type==XIValuatorClass) {
                    XIValuatorClassInfo *axis=(XIValuatorClassInfo*)item;
                    if(axis->number>1&&pointer_lines++<1024)
                        fprintf(pointer_log,"time=%lu event=position source=%d valuator=%d value=%.4f\n",raw->time,raw->sourceid,axis->number,axis->value);
                }
            }
            if(devices)pointer_free_device(devices);
        }
    } else if(e->xcookie.evtype==XI_Motion) {
        XIDeviceEvent *motion=e->xcookie.data;double *value=motion->valuators.values;
        for(int i=0;i<motion->valuators.mask_len*8;++i)if(XIMaskIsSet(motion->valuators.mask,i)) {
            if(i>1&&pointer_lines++<1024)
                fprintf(pointer_log,"time=%lu event=motion source=%d valuator=%d value=%.4f flags=%d\n",motion->time,motion->sourceid,i,*value,motion->flags);
            ++value;
        }
    } else if(e->xcookie.evtype==XI_Enter||e->xcookie.evtype==XI_Leave) {
        XIEnterEvent *cross=e->xcookie.data;
        if(pointer_lines++<1024)fprintf(pointer_log,"time=%lu event=crossing enter=%d mode=%d detail=%d\n",cross->time,e->xcookie.evtype==XI_Enter,cross->mode,cross->detail);
    } else if(e->xcookie.evtype==XI_RawButtonPress||e->xcookie.evtype==XI_RawButtonRelease) {
        XIRawEvent *raw=e->xcookie.data;
        if(pointer_lines++<1024)fprintf(pointer_log,"time=%lu event=button detail=%d down=%d\n",raw->time,raw->detail,e->xcookie.evtype==XI_RawButtonPress);
    }
    fflush(pointer_log);XFreeEventData(d,&e->xcookie);
}
