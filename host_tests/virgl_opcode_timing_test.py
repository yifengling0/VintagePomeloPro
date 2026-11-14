"""Execute the production decoder with deterministic opcode and check costs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=ROOT/'thirdparty/virglrenderer')
source=parser.parse_args().source
text=(source/'src/vrend/vrend_decode.c').read_text()
start=text.index('/* Begin bounded decoder diagnostics.')
end=text.index('\nstatic int vrend_decode_ctx_get_fencing_fd(',start)
production=text[start:end]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "virgl_protocol.h"
static uint64_t now=1000000000ull;
static unsigned clocks,logs,batch_logs,op_logs,switches,dispatches,checks,errors;
static unsigned log_ops[WINEHUA_TEST_MAX];
static int dispatch_error;
static bool switch_ok=true,gl_ok=true;
static int test_clock(clockid_t clock,struct timespec *ts){assert(clock==CLOCK_MONOTONIC);clocks++;ts->tv_sec=now/1000000000ull;ts->tv_nsec=now%1000000000ull;return 0;}
#define clock_gettime test_clock
struct vrend_context {int marker;};
struct virgl_context {unsigned ctx_id;};
struct vrend_decode_ctx {struct virgl_context base;struct vrend_context *grctx;};
static struct vrend_context render_ctx={42};
static struct vrend_decode_ctx ctx={{7},&render_ctx};
static bool vrend_hw_switch_context(struct vrend_context *c,bool force){assert(c==&render_ctx&&force);switches++;now+=100;return switch_ok;}
static bool vrend_check_no_error(struct vrend_context *c){assert(c==&render_ctx);checks++;now+=50;return gl_ok;}
static void vrend_report_buffer_error(struct vrend_context *c,unsigned header){assert(c==&render_ctx);(void)header;errors++;}
static int dispatch(struct vrend_context *c,const uint32_t *buf,uint32_t len){assert(c==&render_ctx);assert(len==(*buf>>16));dispatches++;now+=200;return dispatch_error;}
static int (*decode_table[VIRGL_MAX_COMMANDS])(struct vrend_context *,const uint32_t *,uint32_t)={
 [VIRGL_CCMD_NOP]=dispatch,[VIRGL_CCMD_DRAW_VBO]=dispatch,[VIRGL_CCMD_RESOURCE_INLINE_WRITE]=dispatch};
static const char *vrend_get_comand_name(unsigned op){(void)op;return "test-op";}
static void virgl_warn(const char *format,...){
 logs++;va_list args;va_start(args,format);
 if(strstr(format,"opcode batch"))batch_logs++;
 else {assert(strstr(format,"opcode summary"));unsigned op=va_arg(args,unsigned);assert(op<WINEHUA_TEST_MAX);log_ops[op]++;op_logs++;}
 va_end(args);
}
static void virgl_error(const char *fmt,...){(void)fmt;}
static void dump_command_stream_to_file(const void *buf,size_t size){(void)buf;(void)size;assert(false);}
#define TRACE_FUNC() ((void)0)
#define TRACE_SCOPE_SLOW(...) ((void)0)
#define VREND_DEBUG_ENABLED false
#define dbg_dump_cmd_streams 0
#define vrend_debug(...) false
#define VREND_DEBUG(flag,ctx,fmt,offset,...) ((void)(offset))
'''
tests=r'''
int main(void){
 uint32_t buf[]={VIRGL_CCMD_NOP,(1u<<16)|VIRGL_CCMD_DRAW_VBO,123,(2u<<16)|VIRGL_CCMD_RESOURCE_INLINE_WRITE,11,22};
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf)));
 assert(switches==1&&dispatches==3&&checks==3&&!errors&&!logs);
#if WINEHUA_VIRGL_OPCODE_DIAGNOSTIC
 assert(clocks==15);
 assert(winehua_opcode_batches.calls==1&&winehua_opcode_batches.ns==850&&winehua_opcode_batches.bytes==sizeof(buf));
 assert(winehua_opcode_stats[VIRGL_CCMD_DRAW_VBO].ns==200&&winehua_opcode_stats[VIRGL_CCMD_DRAW_VBO].bytes==8);
 assert(winehua_opcode_stats[VIRGL_CCMD_RESOURCE_INLINE_WRITE].bytes==12);
 assert(winehua_opcode_stats[WINEHUA_OPCODE_CHECK].calls==3&&winehua_opcode_stats[WINEHUA_OPCODE_CHECK].ns==150);
 assert(winehua_opcode_stats[WINEHUA_OPCODE_SWITCH].ns==100);
#else
 assert(!clocks);
#endif
 uint32_t invalid=255;unsigned old=dispatches;
 assert(vrend_decode_ctx_submit_cmd(&ctx.base,&invalid,4)==EINVAL&&dispatches==old);
 uint32_t truncated=(5u<<16)|VIRGL_CCMD_NOP;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,&truncated,4)&&dispatches==old&&errors==1);
 switch_ok=false;assert(vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf))==EINVAL&&dispatches==old);switch_ok=true;
 dispatch_error=ENOMEM;assert(vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf))==ENOMEM&&dispatches==old+1);dispatch_error=0;
 gl_ok=false;assert(vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf))==EINVAL&&dispatches==old+2&&errors==2);gl_ok=true;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,NULL,0));
#if WINEHUA_VIRGL_OPCODE_DIAGNOSTIC
 for(int i=0;i<10;i++)assert(!vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf)));
 assert(!logs);now+=5000000000ull;
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,NULL,0));
 assert(batch_logs==1&&op_logs==5&&logs==6);
 assert(log_ops[VIRGL_CCMD_NOP]==1&&log_ops[WINEHUA_OPCODE_CHECK]==1);
 assert(!winehua_opcode_batches.calls&&!winehua_opcode_stats[VIRGL_CCMD_NOP].calls);
 assert(!vrend_decode_ctx_submit_cmd(&ctx.base,buf,sizeof(buf))&&batch_logs==1);
#else
 assert(!clocks&&!logs);
#endif
 puts("PASS production decoder: opcode/check/switch costs, byte counts, disabled clocks, dispatch/error parity, malformed buffers and bounded summaries");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-opcode-') as tmp:
    tmp=Path(tmp)
    src=tmp/'test.c'
    src.write_text('#define WINEHUA_TEST_MAX 258\n'+prefix+production+tests)
    for mode in (0,1):
        out=tmp/f'test-{mode}'
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
            f'-DWINEHUA_VIRGL_OPCODE_DIAGNOSTIC={mode}','-I',str(source/'src'),
            str(src),'-o',str(out)],check=True)
        subprocess.run([str(out)],check=True,timeout=15)
