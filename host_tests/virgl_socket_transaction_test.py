#!/usr/bin/env python3
"""Compile the real vtest socket source and force concurrent request boundaries.

Only Mesa type/format dependencies are stubbed. The wire parser, socketpair,
threads, partial transactions, replies and forced scheduler delays are real.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[1]
SOCKET = ROOT / 'thirdparty/mesa/src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c'
SHIM = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include "c11/threads.h"
#define ASSERTED
#define WINEHUA_VTEST_MAX_PRESENT_PACERS 16
#include "vtest_protocol.h"
enum pipe_texture_target { TEST_TEXTURE = 2 };
struct pipe_box { int x,y,z,width,height,depth; };
struct virgl_caps_v1 { uint32_t dummy[32]; };
struct virgl_caps_v2 { uint32_t host_feature_check_version; uint32_t dummy[64]; };
struct virgl_drm_caps { union { struct virgl_caps_v1 v1; struct virgl_caps_v2 v2; } caps; };
struct winehua_vtest_present_pacer { uint32_t surface_id; uint64_t next_present_ns; };
struct virgl_vtest_winsys {
    int sock_fd; unsigned protocol_version;
    mtx_t socket_mutex, present_mutex;
    struct winehua_vtest_present_pacer winehua_present_pacers[16];
    uint64_t winehua_present_wait_us, winehua_present_waits;
};
static inline const char *os_get_option(const char *n) { return getenv(n); }
'''
HARNESS = r'''
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
static ssize_t delayed_write(int fd,const void *data,size_t n) {
    ssize_t ret=syscall(SYS_write,fd,data,n);
    if(n==8) usleep(2000); /* expose the real header/body gap */
    return ret;
}
#define write delayed_write
#include "production_socket.c"
#undef write
static struct virgl_vtest_winsys ws;
static int peer;
static atomic_int finished;
static atomic_int pacing_started;
static int mixed_mode;
static unsigned submits, presents, waits, transfers, during_pacing;
static uint64_t pacing_until;
static void bad(const char *why) { fprintf(stderr,"CORRUPTION: %s\n",why); _exit(42); }
static void read_all(int fd,void *data,size_t n) {
    char *p=data;
    while(n) { ssize_t r=read(fd,p,n); if(r<=0) bad("unexpected EOF"); p+=r; n-=r; }
}
static void write_all(int fd,const void *data,size_t n) {
    const char *p=data;
    while(n) { ssize_t r=write(fd,p,n); if(r<=0) bad("write failed"); p+=r; n-=r; }
}
static void *server(void *unused) {
    (void)unused;
    for (;;) {
        uint32_t h[2], body[256];
        ssize_t got=read(peer,h,sizeof(h));
        if(got==0 && atomic_load(&finished)) break;
        if(got<=0) bad("invalid header");
        if(got<sizeof(h)) read_all(peer,(char*)h+got,sizeof(h)-got);
        if(h[0]>256) bad("invalid header");
        read_all(peer,body,h[0]*4);
        if(h[1]==VCMD_SUBMIT_CMD) {
            if(h[0]!=64) bad("submit size");
            for(unsigned i=1;i<64;i++) if(body[i]!=body[0]) bad("interleaved submit body");
            submits++;
            if(winehua_now_ns()<pacing_until) during_pacing++;
        } else if(h[1]==VCMD_WINEHUA_PRESENT) {
            if(h[0]!=VCMD_WINEHUA_PRESENT_SIZE) bad("present size");
            uint32_t serial=body[VCMD_WINEHUA_PRESENT_SERIAL];
            uint32_t reply[VCMD_WINEHUA_PRESENT_REPLY_SIZE]={0};
            h[0]=VCMD_WINEHUA_PRESENT_REPLY_SIZE;
            reply[VCMD_WINEHUA_PRESENT_REPLY_SERIAL]=serial;
            if(!pacing_until) {
                pacing_until=winehua_now_ns()+50000000ull;
                reply[VCMD_WINEHUA_PRESENT_REPLY_STATUS]=1;
                reply[VCMD_WINEHUA_PRESENT_REPLY_DEADLINE_LO]=(uint32_t)pacing_until;
                reply[VCMD_WINEHUA_PRESENT_REPLY_DEADLINE_HI]=pacing_until>>32;
            }
            write_all(peer,h,sizeof(h)); write_all(peer,reply,sizeof(reply)); presents++;
            atomic_store(&pacing_started,1);
        } else if(h[1]==VCMD_RESOURCE_BUSY_WAIT) {
            uint32_t value=body[VCMD_BUSY_WAIT_HANDLE];
            h[0]=1;
            write_all(peer,h,sizeof(h)); write_all(peer,&value,sizeof(value)); waits++;
        } else if(h[1]==VCMD_TRANSFER_PUT) {
            if(h[0]!=VCMD_TRANSFER_HDR_SIZE+16) bad("legacy transfer size");
            for(unsigned i=VCMD_TRANSFER_HDR_SIZE;i<h[0];i++)
                if(body[i]!=0xabcdef01) bad("interleaved legacy transfer data");
            transfers++;
        } else bad("unknown command / stolen header");
    }
    return NULL;
}
static void *submitter(void *arg) {
    uint32_t b[64];
    for(unsigned i=0;i<64;i++) b[i]=(uintptr_t)arg;
    if(mixed_mode) while(!atomic_load(&pacing_started)) usleep(100);
    for(unsigned i=0;i<120;i++) virgl_vtest_submit_cmd(&ws,b,64);
    return NULL;
}
static void *presenter(void *arg) {
    unsigned surface=(uintptr_t)arg;
    for(unsigned i=0;i<30;i++)
        if(virgl_vtest_send_winehua_present(&ws,1,0,0,1,0,16,16,0,i+surface*100, surface))
            bad("wrong present reply");
    return NULL;
}
static void *waiter(void *unused) {
    (void)unused;
    while(!atomic_load(&pacing_started)) usleep(100);
    usleep(60000);
    for(unsigned i=0;i<40;i++)
        if(virgl_vtest_busy_wait(&ws,200+i,0)!=200+i) bad("reply stolen by another reader");
    return NULL;
}
static void *transferer(void *unused) {
    (void)unused;
    while(!atomic_load(&pacing_started)) usleep(100);
    usleep(60000);
    struct pipe_box box={.width=16,.height=1,.depth=1};
    uint32_t data[16];
    for(unsigned i=0;i<16;i++) data[i]=0xabcdef01;
    for(unsigned i=0;i<30;i++) {
        mtx_lock(&ws.socket_mutex);
        virgl_vtest_send_transfer_put(&ws,1,0,64,0,&box,64,0);
        virgl_vtest_send_transfer_put_data(&ws,data,sizeof(data));
        mtx_unlock(&ws.socket_mutex);
    }
    return NULL;
}
int main(int argc,char **argv) {
    int fd[2]; pthread_t s,t[6];
    int mixed=argc>1;
    mixed_mode=mixed;
    (void)argv;
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0);
    ws.sock_fd=fd[0]; peer=fd[1];
    assert(INIT_SOCKET==thrd_success);
    assert(mtx_init(&ws.present_mutex,mtx_plain)==thrd_success);
    pthread_create(&s,NULL,server,NULL);
    pthread_create(&t[0],NULL,submitter,(void*)0xaaaaaaaa);
    pthread_create(&t[1],NULL,submitter,(void*)0xbbbbbbbb);
    if(mixed) {
        pthread_create(&t[2],NULL,presenter,(void*)1);
        pthread_create(&t[3],NULL,presenter,(void*)2);
        pthread_create(&t[4],NULL,waiter,NULL);
        pthread_create(&t[5],NULL,transferer,NULL);
    }
    for(unsigned i=0;i<(mixed?6:2);i++) pthread_join(t[i],NULL);
    atomic_store(&finished,1); shutdown(ws.sock_fd,SHUT_WR); pthread_join(s,NULL);
    assert(submits==240);
    printf("PASS submits=%u presents=%u replies=%u transfers=%u submits_during_pacing=%u\n",
           submits,presents,waits,transfers,during_pacing);
    fflush(stdout);
    if(mixed) assert(presents==61 && waits==40 && transfers==30 && during_pacing>0);
    close(fd[0]); close(fd[1]); mtx_destroy(&ws.socket_mutex); mtx_destroy(&ws.present_mutex);
    return 0;
}
'''


def run(source, expect_corruption=False):
    with tempfile.TemporaryDirectory(prefix='virgl-socket-') as tmp:
        p = Path(tmp)
        (p/'util/format').mkdir(parents=True)
        (p/'production_socket.c').write_bytes(source.read_bytes())
        (p/'virgl_vtest_winsys.h').write_text(SHIM)
        (p/'virgl_vtest_public.h').write_text('')
        (p/'util/u_process.h').write_text('static inline const char *util_get_process_name(void) { return "test"; }\n')
        (p/'util/format/u_format.h').write_text('static inline unsigned util_format_get_nblocksy(unsigned f,unsigned h) { return h; }\nstatic inline unsigned util_format_get_stride(unsigned f,unsigned w) { return w*4; }\n')
        (p/'vtest_protocol.h').write_bytes((ROOT/'thirdparty/mesa/src/virtio/vtest/vtest_protocol.h').read_bytes())
        winsys = SOCKET.with_name('virgl_vtest_winsys.c').read_text()
        init = re.search(r'mtx_init\(&vtws->socket_mutex,\s*([^)]*)\)',winsys).group(0)
        (p/'test.c').write_text(HARNESS.replace('INIT_SOCKET',init.replace('vtws->','ws.')))
        subprocess.run(['cc','-std=c11','-D_GNU_SOURCE','-DHAVE_PTHREAD','-DHAVE_STRUCT_TIMESPEC','-O2','-pthread','-I'+tmp,
                        '-I'+str(ROOT/'thirdparty/mesa/src'),str(p/'test.c'),
                        str(ROOT/'thirdparty/mesa/src/c11/impl/threads_posix.c'),
                        str(ROOT/'thirdparty/mesa/src/c11/impl/time.c'),'-o',str(p/'test')],check=True)
        result = subprocess.run([str(p/'test')]+([] if expect_corruption else ['mixed']),timeout=15,capture_output=True,text=True)
        print(result.stdout+result.stderr,end='')
        assert result.returncode == (42 if expect_corruption else 0), result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-source',type=Path)
    parser.add_argument('--verify-old',action='store_true',help='Reverse patch 0006 in a temporary source tree and require corruption')
    args = parser.parse_args()
    if args.old_source:
        run(args.old_source,True)
        print('Old production source reproduced packet corruption')
    if args.verify_old:
        with tempfile.TemporaryDirectory(prefix='virgl-old-') as tmp:
            directory=Path(tmp)/'src/gallium/winsys/virgl/vtest'
            directory.mkdir(parents=True)
            for name in ['virgl_vtest_socket.c','virgl_vtest_winsys.c','virgl_vtest_winsys.h']:
                shutil.copy2(SOCKET.with_name(name),directory/name)
            subprocess.run(['git','apply','--reverse',str(ROOT/'patches/mesa/0006-virgl-vtest-serialize-socket-transactions.patch')],cwd=tmp,check=True)
            run(directory/'virgl_vtest_socket.c',True)
            print('Old production source reproduced packet corruption')
    run(SOCKET)
