"""Execute the production present bridge with queue and lifetime assertions."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
text=(r/'thirdparty/mesa/src/gallium/drivers/zink/zink_screen.c').read_text()
func=text[text.index('static bool\nzink_winehua_present_frontbuffer('):text.index('static void\nzink_flush_frontbuffer(',text.index('static bool\nzink_winehua_present_frontbuffer('))]
prefix=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define DETECT_OS_WINDOWS 0
#define PIPE_TEXTURE_2D 2
#define VK_DRIVER_ID_MESA_VENUS 23
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 6
#define VK_ACCESS_TRANSFER_READ_BIT 1
#define VK_PIPELINE_STAGE_TRANSFER_BIT 1
typedef uintptr_t VkQueue;
typedef uintptr_t VkImage;
typedef unsigned VkFormat;
typedef unsigned VkImageLayout;
typedef void (*PFN_vkVoidFunction)(void);
struct pipe_resource {unsigned target,nr_samples,width0,height0,format;};
struct object {VkImage image;};
struct zink_resource {struct pipe_resource base;struct object *obj;unsigned layout;};
struct pipe_context {void(*flush)(struct pipe_context*,void*,unsigned);};
struct pipe_screen {int unused;};
struct batch {bool has_work;};
struct zink_context {struct pipe_context base;unsigned winehua_present_serial;struct batch *bs;};
struct zink_screen {struct pipe_screen base;struct {struct {unsigned driverID;} driver_props;} info;void *winehua_present_lib;int queue_lock;const void *winehua_present_surface_page;PFN_vkVoidFunction winehua_present_image;PFN_vkVoidFunction(*vk_GetDeviceProcAddr)(int,const char*);int dev;bool device_lost;void(*image_barrier)(struct zink_context*,struct zink_resource*,unsigned,unsigned,unsigned);VkQueue queue;};
static bool enabled=true,lookup_fail,dl_success;
static int result,phase,flushes,references;
static struct zink_screen *active;
static struct zink_resource *exported;
static struct zink_screen *zink_screen(struct pipe_screen *p){return (struct zink_screen*)p;}
static struct zink_context *zink_tc_context_unwrap(struct pipe_context *p){return (struct zink_context*)p;}
static struct zink_resource *zink_resource(struct pipe_resource *p){return (struct zink_resource*)p;}
static bool util_format_is_depth_or_stencil(unsigned f){return f==99;}
static bool debug_get_bool_option(const char *n,bool d){(void)n;(void)d;return enabled;}
static void simple_mtx_lock(int *m){assert(!*m);*m=1;}
static void simple_mtx_unlock(int *m){assert(*m);*m=0;}
static unsigned zink_get_format(struct zink_screen *s,unsigned f){(void)s;return f+100;}
static void barrier(struct zink_context *c,struct zink_resource *v,unsigned l,unsigned a,unsigned st){(void)c;(void)a;(void)st;assert(phase==0);v->layout=l;exported=v;phase=1;}
static void flush(struct pipe_context *p,void *f,unsigned flags){(void)f;assert(!flags);flushes++;if(phase==1)phase=2;else {assert(phase==4&&((struct zink_context*)p)->bs->has_work);phase=0;}}
static void zink_batch_reference_resource_rw(struct zink_context *c,struct zink_resource *v,bool write){(void)c;assert(!write&&phase==3&&v==exported);references++;phase=4;}
static int present(VkQueue queue,VkImage image,uint32_t w,uint32_t h,VkFormat f,VkImageLayout layout,uint32_t pid,uint32_t surface,uint32_t serial,uint64_t *deadline){assert(phase==2&&active->queue_lock);assert(queue==7&&image==exported->obj->image&&w==exported->base.width0&&h==exported->base.height0&&f==101&&layout==6&&pid==(uint32_t)getpid()&&surface==42&&serial&&deadline);phase=3;return result;}
static PFN_vkVoidFunction getproc(int d,const char *n){(void)d;assert(!strcmp(n,"vn_winehua_present"));return lookup_fail?NULL:(PFN_vkVoidFunction)present;}
static void *util_dl_open(const char *n){assert(!strcmp(n,"libvulkan_virtio.so"));return dl_success?(void*)1:NULL;}
static void *util_dl_get_proc_address(void *l,const char *n){assert(l==(void*)1&&!strcmp(n,"vn_winehua_present"));return (void*)present;}
#define mesa_logi(...) ((void)0)
#define mesa_logw(...) ((void)0)
'''
main=r'''
int main(void){
 struct {uint32_t magic,version,surface,reserved;} page={0x57535053,1,42,0};
 struct zink_screen s={.winehua_present_surface_page=&page,.vk_GetDeviceProcAddr=getproc,.queue=7,.image_barrier=barrier};active=&s;
 struct batch bs={0};struct zink_context c={.base={flush},.bs=&bs};
 struct object object={123},second={456};struct zink_resource input={.base={.target=2,.width0=320,.height0=180,.format=1},.obj=&object};
 assert(!zink_winehua_present_frontbuffer(&s.base,NULL,&input.base,0,0));assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,NULL,0,0));
 enabled=false;assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));enabled=true;
 for(unsigned mode=0;mode<7;mode++){
  struct zink_resource v=input;unsigned l=0,a=0;
  if(mode==0)l=1;if(mode==1)a=1;if(mode==2)v.base.target=3;if(mode==3)v.base.nr_samples=4;if(mode==4)v.base.width0=0;if(mode==5)v.base.height0=0;if(mode==6)v.base.format=99;
  assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&v.base,l,a));
 }
 page.surface=0;assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));page.surface=42;
 s.winehua_present_image=NULL;lookup_fail=true;assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));lookup_fail=false;
 assert(zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(flushes==2&&references==1&&!phase&&exported==&input);
 input.base.width0=640;input.obj=&second;result=-5;assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(references==2&&!phase&&exported->obj==&second);
 result=1;assert(zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));
 s.device_lost=true;assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(phase==2);phase=0;s.device_lost=false;
 char dir[]="/tmp/zink-page-XXXXXX";assert(mkdtemp(dir));assert(!setenv("TMPDIR",dir,1));char path[512];snprintf(path,sizeof(path),"%s/winehua_present_surface_%u.shm",dir,(uint32_t)getpid());s.winehua_present_surface_page=NULL;
 assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));int fd=open(path,O_RDWR|O_CREAT,0600);assert(fd>=0);
 assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));page.magic=0;assert(write(fd,&page,sizeof(page))==sizeof(page));assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));
 page.magic=0x57535053;assert(pwrite(fd,&page,sizeof(page),0)==sizeof(page));assert(zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(s.winehua_present_surface_page);assert(!munmap((void*)s.winehua_present_surface_page,16));close(fd);unlink(path);rmdir(dir);
 s.winehua_present_surface_page=&page;s.winehua_present_image=NULL;lookup_fail=true;dl_success=true;s.info.driver_props.driverID=0;
 assert(!zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(!s.winehua_present_lib);
 s.info.driver_props.driverID=VK_DRIVER_ID_MESA_VENUS;assert(zink_winehua_present_frontbuffer(&s.base,&c.base,&input.base,0,0));assert(s.winehua_present_lib);
 puts("GPU present: direct image/filter/resize/queue ordering/retirement/error/page/ICD guard PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='zink-present-test-',dir=r/'workspace_temp/mini-virgl-performance') as tmp:
 p=Path(tmp);src=p/'test.c';src.write_text(prefix+func+main)
 for name,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined'])]:
  exe=p/name;subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-misleading-indentation','-O1','-g',*flags,str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
