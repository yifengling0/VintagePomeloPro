"""Replay the effective vtest present handler after fence polling selects ctx0.

Use --source with the pinned VirGL tree plus managed patches. Exercises the
production handler, including error replies; this does not certify GPU pixels.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'thirdparty/virglrenderer')
args = parser.parse_args()
path = args.source / 'vtest/vtest_renderer.c'
if not path.is_file():
    parser.error('effective VirGL source is missing; initialize the submodule or pass --source')
source = path.read_text()
start = source.index('int vtest_winehua_present(uint32_t length_dw)')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
handler = source[start:end]
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "vtest_protocol.h"
struct vtest_input { int (*read)(struct vtest_input *, void *, unsigned); };
struct vtest_context { int ctx_id, out_fd; struct vtest_input *input; void *resource_table; };
struct vtest_resource { uint32_t res_id; };
struct virgl_renderer_resource_info {
 uint32_t handle, virgl_format, width, height, tex_id, flags, stride;
};
static struct vtest_context ctx;
static struct vtest_resource resource={42};
static struct virgl_renderer_resource_info info={42,7,320,200,17,0,1280};
static uint32_t request[VCMD_WINEHUA_PRESENT_SIZE], reply_seen[VCMD_WINEHUA_PRESENT_REPLY_SIZE];
static int current, selected, callbacks, writes, submit_ret, info_ret, callback_ret, read_short;
static bool resource_found=true;
static unsigned checks;
static unsigned long long winehua_present_count;
static void *winehua_present_callback_data=&ctx;
#define CHECK(c) do { ++checks; assert(c); } while (0)
static int read_request(struct vtest_input *in, void *dst, unsigned n) {
 (void)in; CHECK(n==sizeof(request)); memcpy(dst,request,n); return read_short ? 0 : (int)n;
}
static struct vtest_context *vtest_get_current_context(void){ return &ctx; }
static void *intptr_to_pointer(uint32_t n){ return (void *)(uintptr_t)n; }
static struct vtest_resource *util_hash_table_get(void *table,void *key){
 (void)table; CHECK((uintptr_t)key==9); return resource_found ? &resource : NULL;
}
static int virgl_renderer_resource_get_info(uint32_t id,struct virgl_renderer_resource_info *out){
 CHECK(id==42); *out=info; return info_ret;
}
static __attribute__((unused)) int virgl_renderer_submit_cmd(void *buffer,int id,int count){
 CHECK(buffer==NULL && count==0); CHECK(id==ctx.ctx_id); CHECK(current==0);
 ++selected; if(!submit_ret) current=id; return submit_ret;
}
static int present(uint32_t tex,uint32_t w,uint32_t h,uint32_t format,uint32_t flags,
 uint64_t drawable,uint32_t serial,uint32_t pid,uint32_t surface,uint32_t options,
 uint64_t *deadline,void *data){
 CHECK(current==ctx.ctx_id); CHECK(tex==17 && w==320 && h==200 && format==7);
 CHECK(flags==0 && drawable==0x100000002ULL && serial==87 && pid==123 && surface==456);
 CHECK(options==0 && data==&ctx); ++callbacks; *deadline=0x200000003ULL; return callback_ret;
}
static __typeof__(&present) winehua_present_callback=present;
static bool winehua_vtest_perf_summary_enabled(void){return false;}
#define winehua_diag(...) ((void)0)
static int report_failure(const char *message,int status){(void)message;return status;}
static int report_failed_call(const char *message,int status){(void)message;return status;}
static int vtest_block_write(int fd,const void *data,unsigned n){
 CHECK(fd==8); ++writes;
 if(writes==1){const uint32_t *header=data; CHECK(n==VTEST_HDR_SIZE*4);
  CHECK(header[VTEST_CMD_ID]==VCMD_WINEHUA_PRESENT && header[VTEST_CMD_LEN]==VCMD_WINEHUA_PRESENT_REPLY_SIZE);
 } else { CHECK(n==sizeof(reply_seen)); memcpy(reply_seen,data,n); }
 return n;
}
'''
driver = r'''
static void reset(int id){
 static struct vtest_input input={read_request};
 ctx=(struct vtest_context){id,8,&input,NULL};
 memset(request,0,sizeof(request)); memset(reply_seen,0,sizeof(reply_seen));
 request[VCMD_WINEHUA_PRESENT_PROTOCOL_VERSION]=VCMD_WINEHUA_PRESENT_VERSION;
 request[VCMD_WINEHUA_PRESENT_RES_HANDLE]=9;
 request[VCMD_WINEHUA_PRESENT_WIDTH]=320; request[VCMD_WINEHUA_PRESENT_HEIGHT]=200;
 request[VCMD_WINEHUA_PRESENT_FORMAT]=7;
 request[VCMD_WINEHUA_PRESENT_DRAWABLE_LO]=2; request[VCMD_WINEHUA_PRESENT_DRAWABLE_HI]=1;
 request[VCMD_WINEHUA_PRESENT_SERIAL]=87; request[VCMD_WINEHUA_PRESENT_CLIENT_PID]=123;
 request[VCMD_WINEHUA_PRESENT_SURFACE_ID]=456;
 // Model dispatch's maintenance poll selecting ctx0, even after the game drew.
 current=0; selected=callbacks=writes=submit_ret=info_ret=callback_ret=read_short=0;
 resource_found=true; winehua_present_callback=present;
 info=(struct virgl_renderer_resource_info){42,7,320,200,17,0,1280};
}
static int invoke(void){return vtest_winehua_present(VCMD_WINEHUA_PRESENT_SIZE);}
static void reply_status(int status){
 CHECK(writes==2); CHECK((int32_t)reply_seen[VCMD_WINEHUA_PRESENT_REPLY_STATUS]==status);
 CHECK(reply_seen[VCMD_WINEHUA_PRESENT_REPLY_SERIAL]==87);
}
int main(void){
 for(int id=11;id<=12;++id){
  reset(id); CHECK(invoke()==0); CHECK(selected==1 && callbacks==1 && current==id);
  reply_status(0); CHECK(reply_seen[VCMD_WINEHUA_PRESENT_REPLY_DEADLINE_LO]==3);
  CHECK(reply_seen[VCMD_WINEHUA_PRESENT_REPLY_DEADLINE_HI]==2);
 }
 for(int status=-EINVAL;status<=EINVAL;status+=2*EINVAL){
  reset(11); submit_ret=status; CHECK(invoke()==0);
  CHECK(selected==1 && callbacks==0 && current==0); reply_status(-EINVAL);
  CHECK(reply_seen[VCMD_WINEHUA_PRESENT_REPLY_DEADLINE_LO]==0);
 }
 reset(11); callback_ret=-EAGAIN; CHECK(invoke()==0); CHECK(callbacks==1); reply_status(-EAGAIN);
 for(int mismatch=0;mismatch<6;++mismatch){
  reset(11);
  if(mismatch==0)info_ret=-EINVAL;
  if(mismatch==1)info.handle=43;
  if(mismatch==2)info.virgl_format=8;
  if(mismatch==3)info.width=321;
  if(mismatch==4)info.height=201;
  if(mismatch==5)info.tex_id=0;
  CHECK(invoke()==0); CHECK(selected==0 && callbacks==0); reply_status(-ENOSYS);
 }
 reset(11); winehua_present_callback=NULL; CHECK(invoke()==0);
 CHECK(selected==0 && callbacks==0); reply_status(-ENOSYS);
 reset(11); resource_found=false; CHECK(invoke()==-ESRCH); CHECK(!selected && !callbacks && !writes);
 reset(11); request[VCMD_WINEHUA_PRESENT_FLAGS]=1;
 CHECK(invoke()==-EINVAL); CHECK(!selected && !callbacks && !writes);
 reset(11); request[VCMD_WINEHUA_PRESENT_PROTOCOL_VERSION]++;
 CHECK(invoke()==-EPROTONOSUPPORT); CHECK(!selected && !callbacks && !writes);
 reset(11); read_short=1; CHECK(invoke()==-1); CHECK(!selected && !callbacks && !writes);
 reset(11); CHECK(vtest_winehua_present(0)==-EINVAL); CHECK(!selected && !callbacks && !writes);
 printf("VirGL production present producer-context/replies: %u checks PASS\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-present-context-') as folder:
    folder = Path(folder)
    (folder / 'test.c').write_text(stub + handler + driver)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(args.source / 'vtest'), str(folder / 'test.c'),
                    '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
