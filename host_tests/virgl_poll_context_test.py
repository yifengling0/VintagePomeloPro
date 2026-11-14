
from pathlib import Path
import subprocess,tempfile
import argparse
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
args=p.parse_args()
source=args.source
text=(source/'src/vrend/vrend_renderer.c').read_text()
body=text[text.index('static bool need_fence_retire_signal_locked('):text.index('\nstatic bool vrend_get_one_query_result(')]
prefix=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "util/list.h"
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE 1
struct vrend_context {void(*fence_retire)(uint64_t,void*);void *fence_retire_data;};
struct vrend_fence {struct list_head fences;struct vrend_context *ctx;uint64_t fence_id;uint32_t flags;bool ready;};
static struct {bool use_async_fence_cb;int sync_thread,eventfd,fence_mutex;struct list_head fence_list;struct vrend_context *current_hw_ctx;} vrend_state;
static unsigned forces,waits,freed,queries,locks,unlocks,flushes,retired;static uint64_t ids[8];
static struct vrend_context contexts[3];
static void retire(uint64_t id,void *data){assert(data==&contexts[1]||data==&contexts[2]);ids[retired++]=id;}
static void vrend_renderer_force_ctx_0(void){forces++;vrend_state.current_hw_ctx=&contexts[0];}
static bool do_wait(struct vrend_fence *f,bool can_block){assert(!can_block);assert(vrend_state.current_hw_ctx);waits++;return f->ready;}
static void free_fence_locked(struct vrend_fence *f){list_del(&f->fences);free(f);freed++;}
static void vrend_renderer_check_queries(void){queries++;vrend_state.current_hw_ctx=&contexts[2];}
static void flush_eventfd(int fd){(void)fd;flushes++;}
static void mtx_lock(int *m){(void)m;locks++;}
static void mtx_unlock(int *m){(void)m;unlocks++;}
static void reset(void){assert(list_is_empty(&vrend_state.fence_list));forces=waits=freed=queries=locks=unlocks=flushes=retired=0;vrend_state.sync_thread=0;vrend_state.current_hw_ctx=&contexts[1];}
static struct vrend_fence *add(unsigned id,int ctx,bool ready,unsigned flags){struct vrend_fence *f=calloc(1,sizeof(*f));assert(f);f->fence_id=id;f->ctx=ctx<0?NULL:&contexts[ctx];f->ready=ready;f->flags=flags;list_addtail(&f->fences,&vrend_state.fence_list);return f;}
"""
tests=r"""
int main(void){
 list_inithead(&vrend_state.fence_list);
 for(int i=0;i<3;i++){contexts[i].fence_retire=retire;contexts[i].fence_retire_data=&contexts[i];}
 reset();vrend_renderer_check_fences();assert(!waits&&!retired&&!queries);
 assert(forces==!WINEHUA_VIRGL_POLL_KEEP_CONTEXT);
 reset();vrend_state.current_hw_ctx=NULL;vrend_renderer_check_fences();assert(forces==1);
 reset();struct vrend_fence *first=add(10,1,false,0);add(11,1,true,0);
 vrend_renderer_check_fences();assert(waits==1&&!retired&&!freed&&!queries);
 first->ready=true;vrend_renderer_check_fences();assert(waits==3&&retired==2&&freed==2&&queries==1&&ids[0]==10&&ids[1]==11);
 assert(forces==(WINEHUA_VIRGL_POLL_KEEP_CONTEXT?0:2));
 reset();add(20,1,true,1);add(21,1,true,0);vrend_renderer_check_fences();assert(retired==1&&ids[0]==21&&freed==2&&waits==2);
 reset();add(30,1,true,1);add(31,2,true,0);vrend_renderer_check_fences();assert(retired==2&&ids[0]==30&&ids[1]==31&&freed==2);
 reset();vrend_state.sync_thread=1;add(40,-1,true,0);add(41,1,true,0);
 vrend_renderer_check_fences();assert(!forces&&!waits&&retired==1&&ids[0]==41&&freed==2&&locks==1&&unlocks==1&&flushes==1);
 reset();puts("PASS production fence polling: valid/null context, FIFO timeout, callbacks, merge, cross-context and threaded parity");
}
"""
with tempfile.TemporaryDirectory(prefix='virgl-poll-context-') as tmp:
 path=Path(tmp);src=path/'test.c';src.write_text(prefix+body+tests)
 for mode in (0,1):
  out=path/('test-'+str(mode))
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-DWINEHUA_VIRGL_POLL_KEEP_CONTEXT='+str(mode),'-I',str(source/'src/mesa'),str(src),'-o',str(out)],check=True)
  subprocess.run([str(out)],check=True,timeout=15)
