#!/usr/bin/env python3
"""Compile production VirGL request serializers and fault-inject their transport."""
import argparse
from pathlib import Path
import os
import re
import resource
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--build-dir',type=Path,required=True)
a=p.parse_args()
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
text=(a.source/'src/gallium/winsys/virgl/vtest/virgl_vtest_socket.c').read_text()
def function(name):
    match=re.search(r'static (?:int|void)\s+'+name+r'\(',text)
    assert match, name
    start=match.start(); body=text.index('{',start); end=body+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]+'\n'
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include "vtest/vtest_protocol.h"
static unsigned char wire[300000];
static size_t used,calls,limit,expected;
static int fail,interrupt_once,reads,locked;
static ssize_t fake_write(int fd,const void *data,size_t size) {
   (void)fd; calls++;
   assert(size<=sizeof(wire)-used);
   if(size)memcpy(wire+used,data,size);
   used+=size;return size;
}
static ssize_t fake_writev(int fd,const struct iovec *parts,int count) {
   (void)fd;calls++;
   if(interrupt_once){interrupt_once=0;errno=EINTR;return -1;}
   if(fail && (fail!=3 || used)) {
      if(fail==1)return 0;
      errno=EIO;return -1;
   }
   size_t n=0;
   for(int i=0;i<count;i++) {
      size_t take=parts[i].iov_len;
      if(limit && take>limit-n)take=limit-n;
      assert(take<=sizeof(wire)-used);
      if(take)memcpy(wire+used,parts[i].iov_base,take);
      used+=take;n+=take;
      if(limit && n==limit)break;
   }
   return n;
}
#define write fake_write
#define writev fake_writev
#include "virgl_vtest_write_pair.h"
#define ASSERTED
struct virgl_vtest_winsys {int sock_fd,socket_mutex;};
static void mtx_lock(int *m){(void)m;assert(!locked);locked=1;}
static void mtx_unlock(int *m){(void)m;assert(locked);locked=0;}
static int virgl_block_read(int fd,void *out,int size) {
   (void)fd; assert(locked && used==expected); reads++;
   memset(out,0,size);return size;
}
static pthread_once_t winehua_vtest_write_once=PTHREAD_ONCE_INIT;
static pthread_once_t winehua_vtest_receipt_once=PTHREAD_ONCE_INIT;
static bool winehua_vtest_packed_write;
'''+'\n'.join(function(n) for n in ('virgl_block_write','winehua_vtest_init_write',
    'winehua_vtest_log_write','winehua_vtest_write_command','virgl_vtest_submit_cmd_unlocked',
    'winehua_present_exchange','virgl_vtest_busy_wait_unlocked'))+r'''
static void reset(void){used=calls=limit=expected=0;fail=interrupt_once=reads=locked=0;}
static void check_submit(size_t n,bool packed) {
   uint32_t payload[16384];for(unsigned i=0;i<16384;i++)payload[i]=0x10203040u+i;
   uint32_t hdr[]={n,VCMD_SUBMIT_CMD};
   struct virgl_vtest_winsys v={0};
   reset();assert(!virgl_vtest_submit_cmd_unlocked(&v,payload,n));
   assert(used==sizeof(hdr)+4*n && !memcmp(wire,hdr,sizeof(hdr)));
   assert(!memcmp(wire+sizeof(hdr),payload,n*4));
   assert(calls==(packed?1:2) && !reads);
}
int main(int argc,char **argv) {
   struct virgl_vtest_winsys v={0};
   bool packed=argc>1 && !strcmp(argv[1],"1");
   if(argc>2){reset();fail=1;locked=1;virgl_vtest_busy_wait_unlocked(&v,3,1);return 4;}
   size_t sizes[]={0,1,2,31,32,33,16384};
   for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)check_submit(sizes[i],packed);
   uint32_t hdr[]={2,VCMD_RESOURCE_BUSY_WAIT},body[]={0x12345678,1};
   reset();locked=1;expected=16;
   assert(!virgl_vtest_busy_wait_unlocked(&v,body[0],body[1]));
   assert(reads==2 && calls==(packed?1:2) && !memcmp(wire,hdr,8) && !memcmp(wire+8,body,8));
   uint32_t ph[2]={VCMD_WINEHUA_PRESENT_SIZE,VCMD_WINEHUA_PRESENT};
   uint32_t command[VCMD_WINEHUA_PRESENT_SIZE],rh[2],reply[VCMD_WINEHUA_PRESENT_REPLY_SIZE];
   for(unsigned i=0;i<VCMD_WINEHUA_PRESENT_SIZE;i++)command[i]=0xfedcba98u+i;
   reset();expected=sizeof(ph)+sizeof(command);
   assert(winehua_present_exchange(&v,ph,command,rh,reply)>0);
   assert(reads==2 && !locked && calls==(packed?1:2));
   assert(!memcmp(wire,ph,sizeof(ph)) && !memcmp(wire+sizeof(ph),command,sizeof(command)));
   for(size_t chunk=1;chunk<=18;chunk++) {
      reset();limit=chunk;interrupt_once=1;
      assert(!virgl_vtest_write_pair(0,hdr,8,body,8));
      assert(used==16 && !memcmp(wire,hdr,8) && !memcmp(wire+8,body,8));
   }
   for(int error=1;error<=3;error++) {
      reset();fail=error;limit=3;
      assert(virgl_vtest_write_pair(0,hdr,8,body,8)==-EIO);
      assert(used==(error==3?3:0));
   }
   reset();assert(!virgl_vtest_write_pair(0,NULL,0,NULL,0) && !calls);
   reset();assert(virgl_vtest_write_pair(0,hdr,(size_t)SSIZE_MAX,body,1)==-EOVERFLOW && !calls);
   if(packed) {
      reset();fail=1;assert(virgl_vtest_submit_cmd_unlocked(&v,body,2)==-EIO && !reads);
      reset();fail=1;assert(winehua_present_exchange(&v,ph,command,rh,reply)==-EIO && !reads && !locked);
   }
   puts("PASS production submit/busy/present byte stream, write counts, reply ordering, short writes, EINTR, zero/error, overflow");
}
'''
a.build_dir.mkdir(parents=True,exist_ok=True)
source=a.build_dir/'virgl-write-test.c'; binary=a.build_dir/'virgl-write-test'
source.write_text(code)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
    '-fno-omit-frame-pointer','-g','-I'+str(a.source/'src/virtio'),
    '-I'+str(a.source/'src/gallium/winsys/virgl/vtest'),str(source),'-pthread','-o',str(binary)],check=True)
for flag in ('0','1','true'):
    subprocess.run([str(binary),flag],env=dict(os.environ,VIRGL_WINEHUA_PACKED_WRITE=flag),check=True)
failure=subprocess.run([str(binary),'1','busy-fail'],env=dict(os.environ,VIRGL_WINEHUA_PACKED_WRITE='1'),capture_output=True)
assert failure.returncode==-6, failure.returncode
print('PASS busy-wait write failure terminates before reply read')
