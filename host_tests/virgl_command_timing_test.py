
from pathlib import Path
import subprocess,tempfile
import argparse
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
args=p.parse_args()
text=(args.source/'vtest/vtest_renderer.c').read_text()
helpers=text[text.index('enum winehua_command_stage {'):text.index('\nstatic void winehua_virgl_log(')]
submit=text[text.index('int vtest_submit_cmd(uint32_t length_dw)'):text.index('\nstruct vtest_transfer_args {')]
prefix=r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now=1000000000ull; static unsigned clocks,summaries,reads,execs,fences;
static int fail_alloc,fail_read,fail_exec;
static uint64_t winehua_busy_now_ns(void){clocks++;return now;}
static void winehua_diag(const char *fmt,...){assert(strstr(fmt,"command summary"));summaries++;}
struct input {int(*read)(struct input*,void*,int);};
struct vtest_context {struct input *input;uint32_t ctx_id;};
static int reader(struct input *in,void *buf,int size){(void)in;reads++;now+=1000000;assert(size==16);memset(buf,7,size);return fail_read?-1:size;}
static struct input input={reader}; static struct vtest_context ctx={&input,3};
static struct {uint32_t max_length;} renderer={1024};
static struct vtest_context *vtest_get_current_context(void){return &ctx;}
static int virgl_renderer_submit_cmd(void *buf,uint32_t id,uint32_t len){execs++;now+=2000000;assert(id==3&&len==4&&((unsigned char*)buf)[0]==7);return fail_exec;}
static void winehua_dump_submit_failure(uint32_t id,void *buf,uint32_t len,int ret){(void)id;(void)buf;(void)len;assert(ret);}
static void vtest_create_implicit_fence(void *r,struct vtest_context *c){assert(r==&renderer&&c==&ctx);fences++;now+=3000000;}
static void *allocate(size_t size){return fail_alloc?NULL:malloc(size);}
#define malloc allocate
"""
tests=r"""
int main(void){
 assert(!vtest_submit_cmd(4)); assert(reads==1&&execs==1&&fences==1);
#if WINEHUA_VTEST_COMMAND_DIAGNOSTIC
 assert(clocks==6);assert(winehua_command_totals[0].ns==1000000);
 assert(winehua_command_totals[1].ns==2000000&&winehua_command_totals[2].ns==3000000);
 assert(winehua_command_totals[0].bytes==16&&winehua_command_totals[1].bytes==16);
#else
 assert(!clocks&&!winehua_command_totals[0].calls);
#endif
 assert(vtest_submit_cmd(257)==-1&&reads==1);
 fail_alloc=1;assert(vtest_submit_cmd(4)==-1&&reads==1);fail_alloc=0;
 fail_read=1;assert(vtest_submit_cmd(4)==-1&&execs==1&&fences==1);fail_read=0;
 fail_exec=1;assert(vtest_submit_cmd(4)==-1&&execs==2&&fences==1);fail_exec=0;
#if WINEHUA_VTEST_COMMAND_DIAGNOSTIC
 memset(winehua_command_totals,0,sizeof(winehua_command_totals));winehua_command_window=0;
 uint64_t a=winehua_command_begin();now+=7;winehua_command_end(WINEHUA_COMMAND_GET,a,0);
 a=winehua_command_begin();now+=11;winehua_command_end(WINEHUA_COMMAND_PUT,a,17);
 assert(winehua_command_totals[3].ns==7&&winehua_command_totals[4].bytes==17);
 for(int i=0;i<20;i++){a=winehua_command_begin();now++;winehua_command_end(WINEHUA_COMMAND_FENCE,a,0);}
 assert(!summaries);now+=5000000000ull;a=winehua_command_begin();now++;winehua_command_end(WINEHUA_COMMAND_FENCE,a,0);
 assert(summaries==3&&!winehua_command_totals[2].calls);
#endif
 puts("PASS actual submit: phase costs, disabled clocks, fence/error parity, bytes and bounded summaries");
}
"""
with tempfile.TemporaryDirectory(prefix='vtest-command-') as tmp:
 path=Path(tmp);src=path/'test.c';src.write_text(prefix+helpers+submit+tests)
 for mode in (0,1):
  out=path/('test-'+str(mode))
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-DWINEHUA_VTEST_COMMAND_DIAGNOSTIC='+str(mode),str(src),'-o',str(out)],check=True)
  subprocess.run([str(out)],check=True,timeout=15)
