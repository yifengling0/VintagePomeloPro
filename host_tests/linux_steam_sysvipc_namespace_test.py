"""Reproduce a full legacy IPC table and isolate successive Steam sessions."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = r'''#define _GNU_SOURCE 1
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
union Arg {int val;};
int main(int argc,char **argv) {
    assert(argc==2);
    const key_t key=0x68700000;
    if(!strcmp(argv[1],"fill")) {
        for(int i=0;i<256;++i) assert(semget(key+i,1,IPC_CREAT|IPC_EXCL|0600)>0);
        assert(semget(IPC_PRIVATE,1,0600)==-1&&errno==ENOSPC);
        // Simulate abrupt exit without IPC_RMID.
        _exit(0);
    }
    if(!strcmp(argv[1],"full")) {
        assert(semget(IPC_PRIVATE,1,0600)==-1&&errno==ENOSPC);return 0;
    }
    if(!strcmp(argv[1],"invalid")) {
        assert(semget(IPC_PRIVATE,1,0600)==-1&&errno==ENOSYS);return 0;
    }
    assert(!strcmp(argv[1],"exercise"));
    // Old namespaces already contain this key; this session must be fresh.
    int sem=semget(key,1,IPC_CREAT|IPC_EXCL|0600);assert(sem>0);
    union Arg value={.val=7};assert(!semctl(sem,0,SETVAL,value));
    int shm=shmget(key,4096,IPC_CREAT|IPC_EXCL|0600);assert(shm>0);
    char *data=shmat(shm,NULL,0);assert(data!=(void*)-1);strcpy(data,"parent");
    pid_t pid=fork();assert(pid>=0);
    if(!pid) {
        assert(semget(key,0,0600)==sem&&semctl(sem,0,GETVAL)==7);
        struct sembuf op={0,-1,IPC_NOWAIT};assert(!semop(sem,&op,1));
        int other=shmget(key,0,0600);assert(other==shm);
        char *view=shmat(other,NULL,0);assert(view!=(void*)-1&&!strcmp(view,"parent"));
        strcpy(view,"child");assert(!shmdt(view));_exit(0);
    }
    int status;assert(waitpid(pid,&status,0)==pid&&status==0);
    assert(semctl(sem,0,GETVAL)==6&&!strcmp(data,"child"));
    assert(!semctl(sem,0,IPC_RMID));assert(!shmctl(shm,IPC_RMID,NULL));assert(!shmdt(data));
    return 0;
}
'''

class SessionNamespaces(unittest.TestCase):
    def test_full_legacy_and_crashed_session_do_not_block_next_session(self):
        with tempfile.TemporaryDirectory(prefix='steam-sysvipc-', dir='/var/tmp') as temporary:
            base = Path(temporary)
            source, runner, library = base/'runner.c', base/'runner', base/'compat.so'
            source.write_text(RUNNER)
            flags = ['-O2','-Wall','-Wextra','-Werror']
            subprocess.run(['cc',*flags,str(source),'-o',str(runner)],check=True)
            subprocess.run(['cc',*flags,'-shared','-fPIC',
                str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/sysvipc_compat.c'),
                '-pthread','-o',str(library)],check=True)
            legacy=base/'persistent';legacy.mkdir(mode=0o700)
            cloud=legacy/'cloud-path-map';cloud.write_text('preserve Cloud and account paths')
            env=os.environ.copy();env.pop('STEAMOHOS_SYSVIPC_ROOT',None)
            env.update(LD_PRELOAD=str(library),STEAMOHOS_IPC_ROOT=str(legacy))
            def run(mode, environment):
                result=subprocess.run([str(runner),mode],env=environment,capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                return result
            self.assertIn('exhausted capacity=256',run('fill',env).stderr)
            before={p.name:p.read_bytes() for p in legacy.iterdir()}
            session_a=env|{'STEAMOHOS_SYSVIPC_ROOT':str(base/'session-a')}
            run('fill',session_a)
            session_b=env|{'STEAMOHOS_SYSVIPC_ROOT':str(base/'session-b')}
            run('exercise',session_b)
            run('full',session_a)
            run('full',env)
            self.assertEqual(before,{p.name:p.read_bytes() for p in legacy.iterdir()})
            run('invalid',env|{'STEAMOHOS_SYSVIPC_ROOT':'relative-invalid'})
            self.assertEqual(cloud.read_text(),'preserve Cloud and account paths')
            print('PASS full legacy table, abrupt-exit namespace, fresh-session IPC, fork sharing and persistent-data preservation')

if __name__=='__main__': unittest.main()
