#!/usr/bin/env python3
"""Execute the real vtest context negotiation functions with a bounded transport stub."""
import argparse, hashlib, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
a=p.parse_args();src=a.source.resolve(strict=True);out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
text=(src/'vtest/vtest_renderer.c').read_text()
def function(name):
 start=text.index('int '+name+'(');end=text.index('{',start);depth=1;end+=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "vtest_protocol.h"
#define UNUSED __attribute__((unused))
#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIRGL_RENDERER_CONTEXT_FLAG_OHOS_NATIVE_MEMORY (1u<<16)
struct vtest_input { int (*read)(struct vtest_input *,void *,int); };
struct vtest_context { struct vtest_input *input; int out_fd; unsigned capset_id; bool context_initialized; uint32_t native_memory_flags; };
static uint32_t input, reply[4];
static int transport_read(struct vtest_input *in,void *ptr,int size) { (void)in;assert(size==4);memcpy(ptr,&input,4);return 4; }
static struct vtest_input channel={transport_read};
static struct vtest_context ctx;
static struct vtest_context *vtest_get_current_context(void) { return &ctx; }
static int vtest_block_write(int fd,const void *ptr,int size) { (void)fd;assert(size==16);memcpy(reply,ptr,16);return size; }
static int vtest_lazy_init_context(struct vtest_context *c) { c->context_initialized=true;return 0; }
"""+function('vtest_get_param')+'\n'+function('vtest_context_init')+r"""
static void reset(void) { memset(&ctx,0,sizeof(ctx));ctx.input=&channel;memset(reply,0,sizeof(reply)); }
static int init(uint32_t flags) { input=flags;return vtest_context_init(1); }
static void query(uint32_t param) { input=param;assert(vtest_get_param(1)==0);assert(reply[0]==2 && reply[1]==VCMD_GET_PARAM); }
int main(void) {
 reset();assert(init(4)==0 && ctx.native_memory_flags==0);
 query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1);assert(!reply[2] && !ctx.native_memory_flags);
 reset();assert(init(4|(1u<<16))==-EINVAL);
 reset();query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1);assert(reply[2]==1 && reply[3]==1 && ctx.native_memory_flags==(1u<<16));
 assert(init(4)==-EINVAL);assert(init(4|(1u<<16))==0);
 query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1);assert(reply[2]==1 && reply[3]==1);
 reset();query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1);assert(init(2|(1u<<16))==-EINVAL);
 reset();assert(init(4|(1u<<17))==-EINVAL);
 reset();query(0x12345678);assert(!reply[2] && !ctx.native_memory_flags);assert(init(4)==0);
 puts("PASS native memory negotiation: legacy, exact version, opt-in, unknown flags, capset and context immutability");
}
"""
file=out/'context.c';binary=out/'context';digest=hashlib.sha256(code.encode()).hexdigest();record=out/'source.sha256'
if not record.exists() or record.read_text()!=digest or not binary.exists():
 file.write_text(code)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-D__OHOS__','-I'+str(src/'vtest'),str(file),'-o',str(binary)],check=True,timeout=30)
 record.write_text(digest)
subprocess.run([str(binary)],check=True,timeout=5)

# Native DMA fd admission exercises the actual proxy, including ordinary SHM validation.
proxy=(src/'src/proxy/proxy_context.c').read_text()
start=proxy.index('static int\nproxy_context_get_blob(');end=proxy.index('{',start)+1;depth=1
while depth:
 depth+=(proxy[end]=='{')-(proxy[end]=='}');end+=1
body=proxy[start:end]
header=(src/'src/virgl_resource.h').read_text();start=header.index('enum virgl_resource_fd_type {');end=header.index('};',start)+2
code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
"""+header[start:end]+r"""
struct virgl_context { int unused; };
struct proxy_context { struct virgl_context base; int socket; bool ohos_native_memory; };
struct virgl_context_blob { int type; union { int fd; } u; unsigned map_info; int vulkan_info; };
#define RENDER_CONTEXT_OP_CREATE_RESOURCE 1
struct render_context_op_create_resource_request { struct { int op; } header; uint32_t res_id; uint64_t blob_id,blob_size; uint32_t blob_flags; };
struct render_context_op_create_resource_reply { int fd_type; unsigned map_info; int vulkan_info; };
static struct render_context_op_create_resource_reply response;
static int seal_fail,size_ok,seal_calls,size_calls,closed,added,send_ok=1,recv_ok=1,fd_count=1;
static bool proxy_socket_send_request(int *s,const void *p,unsigned n) { (void)s;(void)p;(void)n;return send_ok; }
static bool proxy_socket_receive_reply_with_fds(int *s,void *p,unsigned n,int *fd,int cap,int *count) { (void)s;assert(n==sizeof(response));assert(cap==1);memcpy(p,&response,n);*fd=97;*count=fd_count;return recv_ok; }
#define proxy_log(...) ((void)0)
static int add_required_seals_to_fd(int fd) { assert(fd==97);seal_calls++;return seal_fail; }
static bool validate_resource_fd_shm(int fd,uint64_t size) { assert(fd==97 && size==65536);size_calls++;return size_ok; }
static int close(int fd) { assert(fd==97);closed++;return 0; }
static void proxy_context_resource_add(struct proxy_context *c,uint32_t id) { (void)c;assert(id==12);added++; }
"""+body+r"""
static struct proxy_context c;
static struct virgl_context_blob blob;
static void reset(int type,bool native) { memset(&c,0,sizeof(c));memset(&blob,0,sizeof(blob));c.ohos_native_memory=native;response.fd_type=type;response.map_info=3;seal_fail=0;size_ok=1;seal_calls=size_calls=closed=added=0;send_ok=recv_ok=fd_count=1; }
static int get(void) { return proxy_context_get_blob(&c.base,12,6,65536,0,&blob); }
int main(void) {
 reset(VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER,false);assert(get()==-1 && closed==1 && !added && !seal_calls && !size_calls);
 reset(VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER,true);
#if defined(__OHOS__) && defined(ENABLE_SAME_PROCESS_RENDER_SERVER) && defined(ENABLE_RENDER_SERVER_WORKER_THREAD)
 assert(get()==0 && !closed && added==1 && !seal_calls && !size_calls && blob.type==VIRGL_RESOURCE_FD_SHM && blob.u.fd==97);
#else
 assert(get()==-1 && closed==1 && !added && !seal_calls);
#endif
 for(int native=0;native<2;native++) {
  reset(VIRGL_RESOURCE_FD_SHM,native);assert(get()==0 && seal_calls==1 && size_calls==1 && added==1);
  reset(VIRGL_RESOURCE_FD_SHM,native);seal_fail=1;assert(get()==-1 && seal_calls==1 && !size_calls && closed==1 && !added);
  reset(VIRGL_RESOURCE_FD_SHM,native);size_ok=0;assert(get()==-1 && seal_calls==1 && size_calls==1 && closed==1 && !added);
 }
 reset(99,true);assert(get()==-1 && closed==1 && !added);
 reset(VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER,true);fd_count=0;assert(get()==-1 && !added);
 reset(VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER,true);send_ok=0;assert(get()==-1 && !added && !closed);
 reset(VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER,true);recv_ok=0;assert(get()==-1 && !added && !closed);
 puts("PASS proxy DMA fd isolation; ordinary SHM seals/size and error paths preserved");
}
"""
file=out/'proxy.c';file.write_text(code)
variants=[[],['-D__OHOS__','-DENABLE_SAME_PROCESS_RENDER_SERVER'],['-D__OHOS__','-DENABLE_SAME_PROCESS_RENDER_SERVER','-DENABLE_RENDER_SERVER_WORKER_THREAD']]
for index,flags in enumerate(variants):
 binary=out/f'proxy-{index}'
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',*flags,str(file),'-o',str(binary)],check=True,timeout=30)
 subprocess.run([str(binary)],check=True,timeout=5)

# Exercise the real worker export entry point with assertions enabled. Release
# success must not hide a Debug abort on the new negotiated fd type.
renderer=(src/'src/venus/vkr_renderer.c').read_text()
start=renderer.index('bool\nvkr_renderer_create_resource(');end=renderer.index('{',start)+1;depth=1
while depth:
 depth+=(renderer[end]=='{')-(renderer[end]=='}');end+=1
body=renderer[start:end]
code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
"""+header[header.index('enum virgl_resource_fd_type {'):header.index('};',header.index('enum virgl_resource_fd_type {'))+2]+r"""
#define TRACE_FUNC() ((void)0)
struct virgl_resource_vulkan_info { int marker; };
struct virgl_context_blob { enum virgl_resource_fd_type type; union { int fd; } u; uint32_t map_info; struct virgl_resource_vulkan_info vulkan_info; };
struct vkr_context { bool ohos_native_memory; };
static struct vkr_context context;
static enum virgl_resource_fd_type type;
static struct vkr_context *vkr_renderer_lookup_context(uint32_t id) { return id==42 ? &context : NULL; }
static bool vkr_context_create_resource(struct vkr_context *ctx,uint32_t id,uint64_t blob_id,uint64_t size,uint32_t flags,struct virgl_context_blob *out) { (void)ctx;(void)id;(void)blob_id;(void)size;(void)flags; *out=(struct virgl_context_blob){.type=type,.u.fd=97,.map_info=3,.vulkan_info={12}}; return true; }
"""+body+r"""
int main(void) {
 enum virgl_resource_fd_type got;int fd;uint32_t map;struct virgl_resource_vulkan_info info={0};
 context.ohos_native_memory=true;
 const enum virgl_resource_fd_type types[]={VIRGL_RESOURCE_FD_SHM,VIRGL_RESOURCE_FD_DMABUF,VIRGL_RESOURCE_FD_OPAQUE,VIRGL_RESOURCE_FD_OHOS_NATIVE_BUFFER};
 for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++) {
  type=types[i];assert(vkr_renderer_create_resource(42,12,6,65536,0,&got,&fd,&map,&info));
  assert(got==type && fd==97 && map==3);
 }
 assert(info.marker==12);
 assert(!vkr_renderer_create_resource(99,12,6,65536,0,&got,&fd,&map,&info));
 puts("PASS asserted renderer export admits negotiated native GPU fd and legacy fd types");
}
"""
file=out/'export.c';binary=out/'export';file.write_text(code)
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-D__OHOS__',str(file),'-o',str(binary)],check=True,timeout=30)
subprocess.run([str(binary)],check=True,timeout=5)
