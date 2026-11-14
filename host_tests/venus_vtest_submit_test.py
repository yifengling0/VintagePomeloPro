#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise production vtest serialization and stream write recovery."""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
a=p.parse_args()
text=(a.source/'src/virtio/vulkan/vn_renderer_vtest.c').read_text()
def function(name):
    start=text.index('static void\n'+name+'(')
    body=text.index('{',start)
    end=body+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]+'\n'
code=r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#define VIRGL_RENDERER_UNSTABLE_APIS
static unsigned char wire[16384];
static size_t used, calls, limit;
static int interrupt_once, failure;
static ssize_t fake_write(int fd, const void *bytes, size_t size) {
    (void)fd; ++calls;
    if(interrupt_once) {interrupt_once=0;errno=EINTR;return -1;}
    if(failure==1) return 0;
    if(failure==2) {errno=EIO;return -1;}
    if(limit && size>limit)size=limit;
    assert(size<=sizeof(wire)-used);
    memcpy(wire+used,bytes,size);used+=size;return (ssize_t)size;
}
#define write fake_write
#include "vn_vtest_submit.h"
#undef write
#define vn_log(...) ((void)0)
static const char *os_get_option(const char *name) {(void)name;return NULL;}
struct vtest {int sock_fd;bool winehua_packed_submit,winehua_submit_path_logged;};
struct vn_renderer_sync {uint32_t sync_id;};
struct vn_renderer_submit_batch {
    const void *cs_data;size_t cs_size;uint32_t ring_idx,sync_count;
    struct vn_renderer_sync *const *syncs;const uint64_t *sync_values;
};
struct vn_renderer_submit {const struct vn_renderer_submit_batch *batches;uint32_t batch_count;};
""" + '\n'.join(function(n) for n in ('vtest_write','submit_cmd2_sizes','vtest_winehua_log_submit_path','vtest_vcmd_submit_cmd2')) + r"""
static void reset(void) {used=calls=limit=0;interrupt_once=failure=0;memset(wire,0xa5,sizeof(wire));}
static void equivalent(const struct vn_renderer_submit *submit, bool eligible) {
    unsigned char reference[16384];
    struct vtest off={.winehua_packed_submit=false},on={.winehua_packed_submit=true};
    reset();vtest_vcmd_submit_cmd2(&off,submit);
    size_t size=used, old_calls=calls;memcpy(reference,wire,size);
    reset();vtest_vcmd_submit_cmd2(&on,submit);
    assert(used==size && !memcmp(reference,wire,size));
    if(eligible) {assert(old_calls==4 && calls==1);}
    else {assert(calls==old_calls);}
    reset();limit=3;interrupt_once=1;on.winehua_submit_path_logged=false;
    vtest_vcmd_submit_cmd2(&on,submit);
    assert(used==size && !memcmp(reference,wire,size));
}
int main(void) {
    uint32_t payload[64];for(unsigned i=0;i<64;i++)payload[i]=0x10203040u+i;
    struct vn_renderer_submit_batch batch={.cs_data=payload,.ring_idx=0};
    struct vn_renderer_submit submit={.batches=&batch,.batch_count=1};
    const uint32_t rings[]={0,1,UINT32_MAX};
    for(unsigned r=0;r<3;r++)for(size_t n=4;n<=128;n+=4) {
        batch.cs_size=n;batch.ring_idx=rings[r];equivalent(&submit,true);
        reset();struct vtest on={.winehua_packed_submit=true};vtest_vcmd_submit_cmd2(&on,&submit);
        uint32_t header[3];struct vcmd_submit_cmd2_batch parsed;
        memcpy(header,wire,sizeof(header));memcpy(&parsed,wire+sizeof(header),sizeof(parsed));
        assert(header[0]==(28+n)/4 && header[1]==VCMD_SUBMIT_CMD2 && header[2]==1);
        assert(parsed.flags==VCMD_SUBMIT_CMD2_FLAG_RING_IDX && parsed.cmd_offset==7);
        assert(parsed.cmd_size==n/4 && parsed.sync_offset==7+n/4 && !parsed.sync_count);
        assert(parsed.ring_idx==rings[r] && used==36+n && !memcmp(wire+36,payload,n));
    }
    batch.cs_size=0;equivalent(&submit,false);
    batch.cs_size=132;equivalent(&submit,false);
    struct vn_renderer_sync syncs[2]={{1},{UINT32_MAX}};
    struct vn_renderer_sync *refs[2]={&syncs[0],&syncs[1]};
    uint64_t values[2]={0,UINT64_MAX};
    batch.cs_size=32;batch.sync_count=2;batch.syncs=refs;batch.sync_values=values;
    equivalent(&submit,false);
    struct vn_renderer_submit_batch batches[2]={batch,batch};
    submit.batches=batches;submit.batch_count=2;equivalent(&submit,false);
    submit.batch_count=0;equivalent(&submit,false);
    unsigned char packed[VN_VTEST_PACKED_MAX_BYTES];memset(packed,0xcc,sizeof(packed));
    assert(!vn_vtest_pack_single_submit(packed,sizeof(packed),payload,0,0));
    assert(!vn_vtest_pack_single_submit(packed,sizeof(packed),payload,3,0));
    assert(!vn_vtest_pack_single_submit(packed,sizeof(packed),payload,132,0));
    assert(!vn_vtest_pack_single_submit(packed,8,payload,32,0));
    assert(!vn_vtest_pack_single_submit(NULL,sizeof(packed),payload,32,0));
    assert(!vn_vtest_pack_single_submit(packed,sizeof(packed),NULL,32,0));
    for(size_t i=0;i<sizeof(packed);i++)assert(packed[i]==0xcc);
    reset();failure=1;assert(vn_vtest_write_full(0,payload,4)==-1 && errno==EPIPE && calls==1);
    reset();failure=2;assert(vn_vtest_write_full(0,payload,4)==-1 && errno==EIO && calls==1);
    reset();assert(!vn_vtest_write_full(0,NULL,0) && !calls);
    puts("PASS: exact wire bytes; 4-to-1 writes; short writes/EINTR; zero/error; large/multi-batch/sync fallback");
}
"""
out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
c=out/'vtest-submit.c';c.write_text(code)
binary=out/'vtest-submit'
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
    '-fno-omit-frame-pointer','-I',a.source/'src/virtio','-I',a.source/'src/virtio/vulkan',c,'-o',binary],check=True,timeout=30)
subprocess.run([binary],check=True,timeout=15)
