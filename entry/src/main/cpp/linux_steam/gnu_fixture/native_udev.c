// Native Linux guest libudev monitor adapter. Linux device data stays in
// the selected Runtime's real libudev. Only permission-denied uevent monitors
// become quiet, pollable endpoints: OHOS exposes no host udev daemon to apps.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>
struct udev; struct udev_device; struct udev_monitor; struct udev_list_entry;
struct udev_enumerate; struct udev_queue; struct udev_hwdb;
static pthread_once_t real_once=PTHREAD_ONCE_INIT;
static void *real_library;
static void load_real(void) {
    const char *paths[]={"/lib/x86_64-linux-gnu/libudev.so.1","/usr/lib/x86_64-linux-gnu/libudev.so.1"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0])&&!real_library;++i)
        real_library=dlopen(paths[i],RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND);
}
static void *symbol(const char *name) {
    pthread_once(&real_once,load_real);
    void *p=real_library?dlsym(real_library,name):NULL;
    if(!p)errno=ENOSYS;
    return p;
}
#define FORWARD(type,name,params,args,failure) \
 type name params { type (*fn) params=(void*)symbol(#name); return fn?fn args:(failure); }
#define FORWARD_VOID(name,params,args) \
 void name params { void (*fn) params=(void*)symbol(#name); if(fn)fn args; }
#include "native_udev_api.h"
void udev_set_log_fn(struct udev *u,void(*log)(struct udev*,int,const char*,int,const char*,const char*,va_list)) {
    void (*fn)(struct udev*,typeof(log))=(void*)symbol("udev_set_log_fn");if(fn)fn(u,log);
}
struct quiet_monitor { struct udev *context; unsigned references; int fd; };
static pthread_mutex_t monitor_lock=PTHREAD_MUTEX_INITIALIZER;
static struct quiet_monitor *monitors[32];
static int slot(struct udev_monitor *monitor) {
    for(unsigned i=0;i<sizeof(monitors)/sizeof(monitors[0]);++i)
        if(monitors[i]&&(void*)monitors[i]==(void*)monitor)return (int)i;
    return -1;
}
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *context,const char *name) {
    struct udev_monitor *(*fn)(struct udev*,const char*)=(void*)symbol(__func__);
    if(!fn)return NULL;
    struct udev_monitor *result=fn(context,name);int error=errno;
    const char *active=getenv("STEAMOHOS_NATIVE_FEX");
    if(result||!active||strcmp(active,"1")||(error!=EACCES&&error!=EPERM)||
       !context||!name||(strcmp(name,"udev")&&strcmp(name,"kernel"))){errno=error;return result;}
    struct quiet_monitor *m=calloc(1,sizeof(*m));if(!m)return NULL;
    m->fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    if(m->fd<0){error=errno;free(m);errno=error;return NULL;}
    m->context=udev_ref(context);m->references=1;
    if(!m->context){close(m->fd);free(m);return NULL;}
    pthread_mutex_lock(&monitor_lock);int available=-1;
    for(unsigned i=0;i<sizeof(monitors)/sizeof(monitors[0]);++i)if(!monitors[i]){available=i;monitors[i]=m;break;}
    pthread_mutex_unlock(&monitor_lock);
    if(available<0){udev_unref(m->context);close(m->fd);free(m);errno=EMFILE;return NULL;}
    static unsigned logged;
    if(!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
        fprintf(stderr,"STEAMOHOS_UDEV monitor=quiet reason=permission_denied errno=%d device_enumeration=runtime hotplug=unavailable\n",error);
    errno=0;return (void*)m;
}
struct udev_monitor *udev_monitor_ref(struct udev_monitor *m) {
    pthread_mutex_lock(&monitor_lock);int i=slot(m);
    if(i>=0){++monitors[i]->references;pthread_mutex_unlock(&monitor_lock);return m;}
    pthread_mutex_unlock(&monitor_lock);
    struct udev_monitor *(*fn)(struct udev_monitor*)=(void*)symbol(__func__);return fn?fn(m):NULL;
}
struct udev_monitor *udev_monitor_unref(struct udev_monitor *m) {
    pthread_mutex_lock(&monitor_lock);int i=slot(m);
    if(i>=0){struct quiet_monitor *q=monitors[i];int destroy=!--q->references;
        if(destroy)monitors[i]=NULL;
        pthread_mutex_unlock(&monitor_lock);
        if(destroy){udev_unref(q->context);close(q->fd);free(q);}return NULL;}
    pthread_mutex_unlock(&monitor_lock);
    struct udev_monitor *(*fn)(struct udev_monitor*)=(void*)symbol(__func__);return fn?fn(m):NULL;
}
#define MONITOR(type,name,params,args,value,failure) \
 type name params { pthread_mutex_lock(&monitor_lock);int i=slot(m); \
 if(i>=0){type result=(value);pthread_mutex_unlock(&monitor_lock);return result;} \
 pthread_mutex_unlock(&monitor_lock);type(*fn)params=(void*)symbol(#name);return fn?fn args:(failure); }
MONITOR(struct udev*,udev_monitor_get_udev,(struct udev_monitor *m),(m),monitors[i]->context,NULL)
MONITOR(int,udev_monitor_get_fd,(struct udev_monitor *m),(m),monitors[i]->fd,-ENOSYS)
MONITOR(int,udev_monitor_enable_receiving,(struct udev_monitor *m),(m),0,-ENOSYS)
MONITOR(int,udev_monitor_set_receive_buffer_size,(struct udev_monitor *m,int size),(m,size),size>0?0:-EINVAL,-ENOSYS)
MONITOR(int,udev_monitor_filter_add_match_subsystem_devtype,(struct udev_monitor *m,const char *subsystem,const char *devtype),(m,subsystem,devtype),subsystem||devtype?0:-EINVAL,-ENOSYS)
MONITOR(int,udev_monitor_filter_add_match_tag,(struct udev_monitor *m,const char *tag),(m,tag),tag?0:-EINVAL,-ENOSYS)
MONITOR(int,udev_monitor_filter_update,(struct udev_monitor *m),(m),0,-ENOSYS)
MONITOR(int,udev_monitor_filter_remove,(struct udev_monitor *m),(m),0,-ENOSYS)
struct udev_device *udev_monitor_receive_device(struct udev_monitor *m) {
    pthread_mutex_lock(&monitor_lock);int i=slot(m);pthread_mutex_unlock(&monitor_lock);
    if(i>=0){errno=EAGAIN;return NULL;}
    struct udev_device *(*fn)(struct udev_monitor*)=(void*)symbol(__func__);return fn?fn(m):NULL;
}
