"""Exercise real socket FD queries and lsof under the observed OHOS restrictions."""
from pathlib import Path
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture'

RESTRICT_PROC = r'''#define _GNU_SOURCE 1
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
static int socket_path(const char *path) {
    if(strncmp(path,"/proc/",6)||!strstr(path,"/fd/"))return 0;
    char target[128];ssize_t n=readlink(path,target,sizeof(target));
    return n>=8&&!memcmp(target,"socket:[",8);
}
#define STAT(name,type) int name(const char *p,type *s) { \
    int (*next)(const char*,type*)=dlsym(RTLD_NEXT,#name); \
    if(socket_path(p)){errno=ENOENT;return -1;}return next(p,s); }
STAT(stat,struct stat)
STAT(stat64,struct stat64)
#define OPEN(name) FILE *name(const char *p,const char *m) { \
    FILE *(*next)(const char*,const char*)=dlsym(RTLD_NEXT,#name); \
    if(!strncmp(p,"/proc/",6)&&strstr(p,"/net/")){errno=EACCES;return NULL;}return next(p,m); }
OPEN(fopen)
OPEN(fopen64)
'''


class SocketMetadata(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='socket-meta-', dir='/var/tmp')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.base.chmod(0o700)
        self.library = self.base / 'metadata.so'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-pthread', str(FIXTURE / 'socket_metadata.c'), '-o', str(self.library)], check=True)

    def probe(self, program, restricted=False, enabled=True):
        probe = self.base / 'probe'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DSTEAMOHOS_LSOF_PROGRAM=' + json.dumps(str(program)),
                        str(FIXTURE / 'connection_identity.c'), '-ldl', '-o', str(probe)], check=True)
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith('STEAMOHOS_') and k != 'LD_PRELOAD'}
        environment.update(STEAMOHOS_SOCKET_METADATA='1' if enabled else '0',
                           STEAMOHOS_IPC_ROOT=str(self.base), LD_PRELOAD=str(self.library))
        if restricted:
            source = self.base / 'restrict.c'
            source.write_text(RESTRICT_PROC)
            library = self.base / 'restrict.so'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            str(source), '-ldl', '-o', str(library)], check=True)
            environment['LD_PRELOAD'] = str(library) + ':' + str(self.library)
        return subprocess.run([str(probe)], env=environment, text=True,
                              capture_output=True, timeout=12)


    def test_authenticated_tcp_discovery_with_private_proc(self):
        source=self.base/'scan.c';source.write_text(r"""
#define _GNU_SOURCE
#include "socket_query_protocol.h"
#include <assert.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
static int wanted,seen;
static int visit(const struct SteamOhosSocketRecord *r,int fd,void *opaque){
 (void)opaque;if(r->pid==getpid()&&r->descriptor==wanted){struct stat s;assert(!fstat(fd,&s)&&r->inode==s.st_ino);seen++;}return 0;
}
int main(void){
 void (*observe)(int)=dlsym(RTLD_DEFAULT,"SteamOhosObserveSocket");
 int (*next)(pid_t,int,struct SteamOhosSocketRecord*)=dlsym(RTLD_DEFAULT,"SteamOhosNextTcpSocket");
 int (*walk)(SteamOhosSocketVisitor,void*)=dlsym(RTLD_DEFAULT,"SteamOhosVisitTcpSockets");assert(observe&&next&&walk);
 int s=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(s>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(s,(void*)&a,sizeof(a))&&!listen(s,1));wanted=s;observe(s);
 assert(!prctl(PR_SET_DUMPABLE,0));struct SteamOhosSocketRecord record;
 int copy=next(getpid(),0,&record);assert(copy>=0&&record.pid==getpid()&&record.uid==geteuid()&&record.descriptor==s);close(copy);
 assert(next(getpid(),s+1,&record)<0&&errno==ENOENT);
 assert(walk(visit,0)>=1&&seen==1);close(s);
 assert(next(getpid(),s,&record)<0&&errno==ENOENT);puts("AUTHENTICATED_TCP_DISCOVERY_REAL_FD_PRIVATE_PROC");return 0;
}
""")
        program=self.base/'scan'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(FIXTURE),str(source),'-ldl','-o',str(program)],check=True)
        environment={**os.environ,'STEAMOHOS_SOCKET_METADATA':'1','STEAMOHOS_IPC_ROOT':str(self.base),'LD_PRELOAD':str(self.library)}
        result=subprocess.run([program],env=environment,text=True,capture_output=True,timeout=12)
        self.assertEqual(result.returncode,0,result.stderr);self.assertIn('AUTHENTICATED_TCP_DISCOVERY_REAL_FD_PRIVATE_PROC',result.stdout)

    @unittest.skipUnless(Path('/bin/lsof').is_file(), 'native host lsof is required')
    def test_live_descriptor_and_stale_inode(self):
        result = self.probe('/bin/lsof')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('role=live-query-child-client', result.stderr)
        self.assertIn('stale_query fd=-1 errno=116', result.stderr)
        self.assertIn('parent_found=1 child_found=1', result.stderr)

    def test_lifecycle_and_peer_identity(self):
        program = self.base / 'lifecycle'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'host_tests/linux_steam_socket_metadata_lifecycle.c'),
                        '-ldl', '-o', str(program)], check=True)
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith('STEAMOHOS_') and k != 'LD_PRELOAD'}
        session = self.base / 'session'
        session.mkdir(mode=0o700)
        environment.update(STEAMOHOS_SOCKET_METADATA='1',
                           STEAMOHOS_IPC_ROOT=str(self.base),
                           STEAMOHOS_SOCKET_METADATA_ROOT=str(session), LD_PRELOAD=str(self.library))
        result = subprocess.run([str(program)], env=environment, text=True,
                                capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('queries=512 fragmented_query=1 fd_leaks=0', result.stdout)
        self.assertIn('wrong_peer_rejected=1 exit_cleanup=1', result.stdout)
        self.assertFalse((self.base / 'socket-metadata').exists())
        self.assertEqual(list((session / 'socket-metadata').iterdir()), [])

    def test_fragmented_response_and_ancillary_validation(self):
        program = self.base / 'stream'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'host_tests/linux_steam_socket_metadata_stream.c'),
                        '-ldl', '-o', str(program)], check=True)
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith('STEAMOHOS_') and k != 'LD_PRELOAD'}
        environment.update(STEAMOHOS_SOCKET_METADATA='1',
                           STEAMOHOS_IPC_ROOT=str(self.base), LD_PRELOAD=str(self.library))
        result = subprocess.run([str(program)], env=environment, text=True,
                                capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fragmented_response=1 duplicate_fd_rejected=1 incomplete_response_rejected=1', result.stdout)

    def test_cooperative_process_and_terminal_worker_shutdown(self):
        program = self.base / 'process'
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'entry/src/main/cpp'),
                        str(ROOT/'host_tests/linux_steam_process_metadata.cpp'), '-ldl', '-pthread', '-o', str(program)], check=True)
        environment = {**os.environ, 'STEAMOHOS_SOCKET_METADATA': '1', 'STEAMOHOS_IPC_ROOT': str(self.base),
                       'STEAMOHOS_SOCKET_METADATA_ROOT': str(self.base), 'LD_PRELOAD': str(self.library)}
        result = subprocess.run([str(program), str(self.base)], env=environment, text=True, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('terminal owned-worker shutdown', result.stdout)

    @unittest.skipUnless(os.environ.get('STEAMOHOS_LSOF_SOURCE_ARCHIVE'), 'pinned lsof source archive is required')
    def test_restricted_proc_requires_real_socket_backend(self):
        output = self.base / 'lsof-build'
        subprocess.run(['python3', str(ROOT / 'scripts/build_linux_steam_lsof.py'),
                        '--archive', os.environ['STEAMOHOS_LSOF_SOURCE_ARCHIVE'],
                        '--cc', '/usr/bin/gcc', '--native-host-test', '--output', str(output)],
                       capture_output=True, text=True, check=True)
        program = output / 'steamohos_lsof'
        result = self.probe(program, restricted=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('table path=/proc/net/tcp open_errno=13', result.stderr)
        self.assertIn('stat=-1/2', result.stderr)
        self.assertIn('parent_found=1 child_found=1', result.stderr)
        denied = self.probe(program, restricted=True, enabled=False)
        self.assertEqual(denied.returncode, 1, denied.stderr)
        self.assertIn('parent_found=0 child_found=0', denied.stderr)


if __name__ == '__main__':
    unittest.main()
