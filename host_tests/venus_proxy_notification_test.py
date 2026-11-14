#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run locked proxy setup with a real eventfd and idle-client poll.

The production initializer and fencing function are extracted, not duplicated.
Host wakeup/ownership evidence only; GPU completion is verified on device.
"""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
a=p.parse_args()
def function(path,signature):
    text=path.read_text()
    start=text.index(signature)
    return text[start:text.index('\n}',start)+3]
initializer=function(a.source/'src/proxy/proxy_renderer.c','int\nproxy_renderer_init(')
fencing=function(a.source/'src/proxy/proxy_context.c','static bool\nproxy_context_init_fencing(')
code=r'''
#define _GNU_SOURCE 1
#define __OHOS__ 1
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#define VIRGL_RENDERER_NO_VIRGL (1u<<0)
#define VIRGL_RENDERER_THREAD_SYNC (1u<<1)
#define VIRGL_RENDERER_ASYNC_FENCE_CB (1u<<2)
#define thrd_success 0
struct proxy_renderer_cbs { int unused; };
struct proxy_context { struct { int fence_eventfd,thread;bool created; } sync_thread; };
static struct {const struct proxy_renderer_cbs *cbs;uint32_t flags;void *server,*client;} proxy_renderer;
static unsigned failures,closed_fds;
static bool fail_eventfd,fail_server,fail_client;
static void *proxy_server_create(void) {return fail_server?NULL:(void*)1;}
static void *proxy_client_create(void *server,uint32_t flags) {
    assert(server && flags==proxy_renderer.flags);return fail_client?NULL:(void*)2;
}
static void proxy_renderer_fini(void) {++failures;memset(&proxy_renderer,0,sizeof(proxy_renderer));}
static void proxy_log(const char *text) {(void)text;}
static int create_eventfd(unsigned value) {return fail_eventfd?-1:eventfd(value,EFD_CLOEXEC|EFD_NONBLOCK);}
static int proxy_context_sync_thread(void *p) {(void)p;return 0;}
static int thrd_create(int *t,int(*fn)(void*),void *data) {(void)t;(void)fn;(void)data;assert(0);return -1;}
'''+initializer+'\n'+fencing+r'''
int main(void) {
    struct proxy_renderer_cbs cbs={0};
    const uint32_t gl_flags=VIRGL_RENDERER_NO_VIRGL;
    for(unsigned i=0;i<128;++i) {
        assert(!proxy_renderer_init(&cbs,gl_flags));
        assert(gl_flags==VIRGL_RENDERER_NO_VIRGL); // caller GL policy unchanged
        assert(proxy_renderer.flags&VIRGL_RENDERER_THREAD_SYNC);
        struct proxy_context ctx={.sync_thread={.fence_eventfd=-1}};
        assert(proxy_context_init_fencing(&ctx));
        assert(ctx.sync_thread.fence_eventfd>=0 && !ctx.sync_thread.created);
        struct pollfd pfd={.fd=ctx.sync_thread.fence_eventfd,.events=POLLIN};
        assert(poll(&pfd,1,0)==0);
        uint64_t value=1;
        assert(write(pfd.fd,&value,sizeof(value))==sizeof(value));
        // No socket command or periodic tick is needed to observe completion.
        assert(poll(&pfd,1,25)==1 && (pfd.revents&POLLIN));
        assert(read(pfd.fd,&value,sizeof(value))==sizeof(value) && value==1);
        assert(poll(&pfd,1,0)==0);
        assert(!close(pfd.fd));++closed_fds;
    }
    fail_eventfd=true;
    struct proxy_context ctx={.sync_thread={.fence_eventfd=-1}};
    assert(!proxy_context_init_fencing(&ctx) && ctx.sync_thread.fence_eventfd==-1);
    fail_eventfd=false;fail_server=true;
    assert(proxy_renderer_init(&cbs,gl_flags)==-1 && failures==1 && !proxy_renderer.server);
    fail_server=false;fail_client=true;
    assert(proxy_renderer_init(&cbs,gl_flags)==-1 && failures==2 && !proxy_renderer.client);
    assert(closed_fds==128);
    puts("PASS: proxy-only notifications, idle poll wake, 128 close cycles, setup errors preserved");
}
'''
out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
source=out/'proxy-notification.c';source.write_text(code)
binary=out/'proxy-notification'
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',source,'-o',binary],check=True,timeout=30)
subprocess.run([binary],check=True,timeout=10)
