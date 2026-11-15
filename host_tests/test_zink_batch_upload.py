"""Exercise the production queue/drain functions with shadow byte snapshots."""
from pathlib import Path
import subprocess,tempfile,re
r=Path(__file__).resolve().parents[1]; z=r/'thirdparty/mesa/src/gallium/drivers/zink'
txt=(z/'zink_batch.c').read_text()
end_batch=txt[txt.index('void\nzink_end_batch('):txt.index('ALWAYS_INLINE static void\nbatch_hashlist_update')]
assert end_batch.index('zink_winehua_flush_mapped_uploads(screen, bs)') < end_batch.index('util_queue_add_job(')
assert re.search(r'if \(zink_winehua_flush_mapped_uploads\(screen, bs\) != VK_SUCCESS\) \{\s*screen->device_lost = true;\s*return;',end_batch)
resource=(z/'zink_resource.c').read_text()
assert 'if ((usage & PIPE_MAP_READ) && !(usage & PIPE_MAP_THREAD_SAFE)' in resource
eligibility=resource.split('const bool private_upload = ',1)[1].split(';',1)[0]
for before,after in [('m->obj->is_buffer','is_buffer'),('ptrans->resource->target','target'),('m->obj->exportable','exportable'),('m->base.b.flags','flags')]:
 eligibility=eligibility.replace(before,after)
a=txt.index('bool\nzink_winehua_queue_mapped_flush');b=txt.index('/* this resets the batch usage',a);func=txt[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t VkDeviceSize;
typedef int VkResult;
enum {VK_SUCCESS=0,VK_ERROR_DEVICE_LOST=-4};
#define VK_WHOLE_SIZE UINT64_MAX
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct memory {unsigned maps;uint8_t guest[1024],host[1024];};
typedef struct {int sType;void *pNext;struct memory *memory;uint64_t offset,size;} VkMappedMemoryRange;
struct zink_screen {int dev;};
struct zink_resource_object {unsigned refs;struct memory *bo;};
struct util_dynarray {void *data;unsigned size,capacity;};
struct zink_winehua_mapped_flush {struct zink_resource_object *obj;VkMappedMemoryRange range;};
struct zink_batch_state {struct util_dynarray winehua_mapped_flushes;bool is_device_lost;};
struct zink_context {struct {struct zink_screen *screen;} base;struct zink_batch_state *bs;};
static bool enabled=true,map_fail,grow_fail,flush_fail;
static unsigned calls,ranges_seen;static VkMappedMemoryRange emitted[9000];
static void *grow(struct util_dynarray *a,unsigned size) {
 if(grow_fail)return NULL;
 if(a->size+size>a->capacity){unsigned cap=a->capacity?2*a->capacity:4096;while(cap<a->size+size)cap*=2;void *p=realloc(a->data,cap);assert(p);a->data=p;a->capacity=cap;}
 void *p=(char*)a->data+a->size;a->size+=size;return p;
}
#define util_dynarray_num_elements(a,t) ((a)->size/sizeof(t))
#define util_dynarray_top_ptr(a,t) ((t*)((char*)(a)->data+(a)->size-sizeof(t)))
#define util_dynarray_grow(a,t,n) grow(a,sizeof(t)*(n))
#define util_dynarray_clear(a) ((a)->size=0)
static bool debug_get_bool_option(const char *s,bool d){(void)s;(void)d;return enabled;}
static struct zink_screen *zink_screen(struct zink_screen *s){return s;}
static void *zink_bo_map(struct zink_screen *s,struct memory *m){(void)s;if(map_fail)return NULL;m->maps++;return m->guest;}
static void zink_bo_unmap(struct zink_screen *s,struct memory *m){(void)s;assert(m->maps);m->maps--;}
static void zink_resource_object_reference(struct zink_screen *s,struct zink_resource_object **dst,struct zink_resource_object *src){(void)s;if(src)src->refs++;if(*dst){assert((*dst)->refs);(*dst)->refs--;}*dst=src;}
static int flush(int dev,unsigned n,const VkMappedMemoryRange *v){(void)dev;assert(n<=256);calls++;if(flush_fail)return VK_ERROR_DEVICE_LOST;for(unsigned i=0;i<n;i++){assert(v[i].memory->maps);assert(v[i].offset+v[i].size<=1024);emitted[ranges_seen++]=v[i];memcpy(v[i].memory->host+v[i].offset,v[i].memory->guest+v[i].offset,v[i].size);}return VK_SUCCESS;}
#define VKSCR(n) flush
#define mesa_loge(...) ((void)0)
VkResult zink_winehua_flush_mapped_uploads(struct zink_screen *,struct zink_batch_state *);
'''
prefix += r'''
enum {PIPE_BUFFER=0,PIPE_TEXTURE_2D=2};
enum {PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE=1};
enum {PIPE_MAP_WRITE=1,PIPE_MAP_UNSYNCHRONIZED=2,PIPE_MAP_FLUSH_EXPLICIT=4,
PIPE_MAP_READ=8,PIPE_MAP_PERSISTENT=16,PIPE_MAP_ONCE=32,PIPE_MAP_THREAD_SAFE=64,
TC_TRANSFER_MAP_THREADED_UNSYNC=128};
static bool eligible(bool remote_upload,bool is_buffer,unsigned target,bool exportable,unsigned flags,unsigned map_flags) {
 return ''' + eligibility + r''';
}
static void test_eligibility(void) {
 const unsigned required=PIPE_MAP_WRITE|PIPE_MAP_UNSYNCHRONIZED|PIPE_MAP_FLUSH_EXPLICIT;
 assert(eligible(true,true,PIPE_BUFFER,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required));
 for(unsigned bit=1;bit<=PIPE_MAP_FLUSH_EXPLICIT;bit<<=1)
  assert(!eligible(true,true,PIPE_BUFFER,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required&~bit));
 for(unsigned bit=PIPE_MAP_READ;bit<=TC_TRANSFER_MAP_THREADED_UNSYNC;bit<<=1)
  assert(!eligible(true,true,PIPE_BUFFER,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required|bit));
 assert(!eligible(false,true,PIPE_BUFFER,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required));
 assert(!eligible(true,false,PIPE_BUFFER,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required));
 assert(!eligible(true,true,PIPE_TEXTURE_2D,false,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required));
 assert(!eligible(true,true,PIPE_BUFFER,true,PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE,required));
 assert(!eligible(true,true,PIPE_BUFFER,false,0,required));
}
'''
main=r'''
static void queue(struct zink_context *c,struct zink_resource_object *o,unsigned off,unsigned n){VkMappedMemoryRange r={0,NULL,o->bo,off,n};assert(zink_winehua_queue_mapped_flush(c,o,&r));}
int main(void){
 test_eligibility();
 struct zink_screen screen={0};struct zink_batch_state bs={0};struct zink_context c={{&screen},&bs};
 struct memory a={0},b={0};struct zink_resource_object x={1,&a},y={1,&b};
 memset(a.guest,0x17,1024);memset(a.host,0x99,1024);memset(b.guest,0x42,1024);
 queue(&c,&x,32,16);queue(&c,&x,40,16);queue(&c,&x,56,8);queue(&c,&x,80,8);queue(&c,&y,32,16);
 assert(calls==0&&a.host[32]==0x99&&x.refs==3&&a.maps==2);
 assert(zink_winehua_flush_mapped_uploads(&screen,&bs)==VK_SUCCESS);
 assert(calls==1&&ranges_seen==3&&a.host[32]==0x17&&a.host[63]==0x17&&a.host[64]==0x99&&a.host[79]==0x99&&a.host[80]==0x17&&b.host[32]==0x42);
 assert(x.refs==1&&y.refs==1&&!a.maps&&!b.maps&&!bs.winehua_mapped_flushes.size);
 /* Object rename: retain and flush the captured old backing. */
 queue(&c,&x,0,4);struct zink_resource_object *renamed=&y;(void)renamed;
 zink_winehua_flush_mapped_uploads(&screen,&bs);assert(a.host[0]==0x17&&b.host[0]==0);
 /* Reverse input order merges; gaps remain separate. */
 unsigned prev=ranges_seen;queue(&c,&x,100,4);queue(&c,&x,96,4);queue(&c,&x,110,2);
 zink_winehua_flush_mapped_uploads(&screen,&bs);assert(ranges_seen-prev==2);
 /* All pending memory references and mappings survive more than one call. */
 struct memory *mem=calloc(520,sizeof(*mem));struct zink_resource_object *obj=calloc(520,sizeof(*obj));
 for(unsigned i=0;i<520;i++){obj[i]=(struct zink_resource_object){1,&mem[i]};mem[i].guest[0]=(uint8_t)i;queue(&c,&obj[i],0,1);}
 prev=calls;zink_winehua_flush_mapped_uploads(&screen,&bs);assert(calls-prev==3);
 for(unsigned i=0;i<520;i++){assert(mem[i].host[0]==(uint8_t)i&&obj[i].refs==1&&!mem[i].maps);}free(obj);free(mem);
 /* Bounded metadata drains early without exposing a later batch. */
 for(unsigned i=0;i<4100;i++)queue(&c,(i&1)?&x:&y,(i&1)?200:300,1);
 assert(bs.winehua_mapped_flushes.size/sizeof(struct zink_winehua_mapped_flush)==4);
 zink_winehua_flush_mapped_uploads(&screen,&bs);assert(x.refs==1&&y.refs==1&&!a.maps&&!b.maps);
 /* Opt-out and allocation failure preserve caller's immediate fallback. */
 VkMappedMemoryRange r={0,NULL,&a,0,1};enabled=false;assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));enabled=true;
 map_fail=true;assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));map_fail=false;
 grow_fail=true;assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));grow_fail=false;assert(x.refs==1&&!a.maps);
 queue(&c,&x,0,4);flush_fail=true;assert(zink_winehua_flush_mapped_uploads(&screen,&bs)==VK_ERROR_DEVICE_LOST);assert(bs.is_device_lost&&x.refs==1&&!a.maps&&!bs.winehua_mapped_flushes.size);
 assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));
 assert(zink_winehua_flush_mapped_uploads(&screen,&bs)==VK_ERROR_DEVICE_LOST);
 /* An early capacity drain failure cannot be erased by a later empty drain. */
 bs.is_device_lost=false;flush_fail=false;
 for(unsigned i=0;i<4096;i++)queue(&c,(i&1)?&x:&y,(i&1)?200:300,1);
 flush_fail=true;assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));
 assert(bs.is_device_lost&&!bs.winehua_mapped_flushes.size&&x.refs==1&&y.refs==1&&!a.maps&&!b.maps);
 assert(zink_winehua_flush_mapped_uploads(&screen,&bs)==VK_ERROR_DEVICE_LOST);
 assert(!zink_winehua_queue_mapped_flush(&c,&x,&r));
 free(bs.winehua_mapped_flushes.data);puts("batch snapshots/ranges/lifetimes/failures PASS");return 0;
}
'''
work=r/'workspace_temp/mini-virgl-performance';work.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='zink-upload-test-',dir=work) as tmp:
 p=Path(tmp);src=p/'test.c';src.write_text(prefix+func+main)
 for name,flags in [('normal',[]),('sanitized',['-fsanitize=address,undefined'])]:
  exe=p/name;subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g',*flags,str(src),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
 print('production helpers + ASAN/UBSAN PASS')
