"""Validate real private /tmp files and GNU-native executable identity."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Filesystem(unittest.TestCase):
    def test_private_tmp_preserves_io_and_errors(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-filesystem-", dir="/var/tmp") as temporary:
            base = Path(temporary)
            private = base / "private-tmp"
            private.mkdir(mode=0o700)
            source = base / "runner.c"
            source.write_text(r'''#define _GNU_SOURCE 1
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <linux/openat2.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
extern int __xstat64(int,const char*,struct stat64*);
int main(int argc,char **argv) {
    assert(argc==3);
    assert(syscall(SYS_getpid)==getpid());
    assert(!setenv("STEAMOHOS_NATIVE_MEMORY","1",1));
    assert(syscall(SYS_landlock_create_ruleset,NULL,0,1)==-1&&errno==ENOSYS);
    int bulk_fd=open("/dev/null",O_RDONLY);assert(bulk_fd>=0);
    assert(syscall(SYS_close_range,bulk_fd,bulk_fd,0)==-1&&errno==ENOSYS);
    assert(fcntl(bulk_fd,F_GETFD)>=0);assert(!close(bulk_fd));
    assert(fcntl(bulk_fd,F_GETFD)==-1&&errno==EBADF);
    struct open_how how={.flags=O_RDONLY|O_CLOEXEC};
    assert(syscall(SYS_openat2,AT_FDCWD,"/dev/null",&how,sizeof(how))==-1&&errno==ENOSYS);
    // The caller chooses its fallback; the adapter never claims resolve flags
    // were enforced by the older API.
    int ordinary=openat(AT_FDCWD,"/dev/null",O_RDONLY|O_CLOEXEC);assert(ordinary>=0);assert(!close(ordinary));
    how.resolve=RESOLVE_BENEATH;
    assert(syscall(SYS_openat2,AT_FDCWD,"/dev/null",&how,sizeof(how))==-1&&errno==ENOSYS);
    assert(!unsetenv("STEAMOHOS_NATIVE_MEMORY"));
    how.resolve=0;
    ordinary=syscall(SYS_openat2,AT_FDCWD,"/dev/null",&how,sizeof(how));assert(ordinary>=0);assert(!close(ordinary));
    bulk_fd=open("/dev/null",O_RDONLY);assert(bulk_fd>=0);
    assert(!syscall(SYS_close_range,bulk_fd,bulk_fd,0));
    assert(fcntl(bulk_fd,F_GETFD)==-1&&errno==EBADF);
    void *raw=(void*)syscall(SYS_mmap,NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(raw!=MAP_FAILED);memset(raw,0x67,4096);assert(!syscall(SYS_munmap,raw,4096));
    char directory[256],path[280],other[280];
    snprintf(directory,sizeof(directory),"/tmp/steamohos-fs-%d",(int)getpid());
    assert(!mkdir(directory,0700));
    assert(mkdir(directory,0700)==-1 && errno==EEXIST);
    struct stat info;assert(!stat(directory,&info) && info.st_uid==geteuid() && S_ISDIR(info.st_mode));
    snprintf(path,sizeof(path),"%s/one",directory);
    snprintf(other,sizeof(other),"%s/two",directory);
    int fd=open64(path,O_RDWR|O_CREAT|O_EXCL,0600);assert(fd>=0);
    assert(write(fd,"real-data",9)==9);assert(!close(fd));
    FILE *file=fopen64(path,"r");assert(file);char data[32]={0};assert(fread(data,1,9,file)==9&&!strcmp(data,"real-data"));assert(!fclose(file));
    struct stat64 legacy;
#if defined(__aarch64__)
    const int version=0;
#else
    const int version=1;
#endif
    assert(!__xstat64(version,path,&legacy)&&legacy.st_size==9);
    int dir=open(directory,O_RDONLY|O_DIRECTORY);assert(dir>=0);
    fd=openat(dir,"one",O_RDONLY);assert(fd>=0);assert(!close(fd));assert(!close(dir));
    assert(!rename(path,other));assert(access(path,F_OK)==-1&&errno==ENOENT);
    struct statvfs space;assert(!statvfs(other,&space));
    assert(!unlink(other));assert(!rmdir(directory));
    assert(stat(directory,&info)==-1&&errno==ENOENT);
    char detached[]="/var/tmp/steamohos-detached-XXXXXX";assert(mkdtemp(detached));
    dir=open(detached,O_RDONLY|O_DIRECTORY);assert(dir>=0);assert(!rmdir(detached));
    assert(!fstatat(dir,".",&info,0)&&S_ISDIR(info.st_mode));
    fd=openat(dir,".",O_RDONLY|O_DIRECTORY);assert(fd>=0);close(fd);close(dir);
    file=fopen("/tmp/steamohos-logical-output","w");assert(file);assert(fputs("private-output",file)>=0);assert(!fclose(file));
    file=fopen(argv[2],"w");assert(file);assert(fputs("unrelated-output",file)>=0);assert(!fclose(file));
    char temporary[]="/tmp/steamohos-created-XXXXXX";
    fd=mkstemp(temporary);assert(fd>=0);assert(!close(fd));
    assert(!strncmp(temporary,"/tmp/steamohos-created-",22));assert(!stat(temporary,&info));assert(!unlink(temporary));
    char suffix[]="/tmp/steamohos-created-XXXXXX.log";
    fd=mkstemps(suffix,4);assert(fd>=0);assert(!close(fd));assert(!strcmp(suffix+strlen(suffix)-4,".log"));assert(!unlink(suffix));
    char temporary_directory[]="/tmp/steamohos-dir-XXXXXX";
    assert(mkdtemp(temporary_directory)==temporary_directory);assert(!stat(temporary_directory,&info));assert(!rmdir(temporary_directory));
    char link[4096]={0};ssize_t size=readlink("/proc/self/exe",link,sizeof(link));
    assert(size==(ssize_t)strlen(argv[1])&&!memcmp(link,argv[1],size));
    memset(link,0,sizeof(link));size=readlinkat(AT_FDCWD,"/proc/self/exe",link,3);
    assert(size==3&&!memcmp(link,argv[1],3));
    assert(readlink("/proc/self/exe",link,0)==-1&&errno==EINVAL);
    FILE *roots=fopen("/etc/ssl/certs/ca-certificates.crt","r");assert(roots);
    memset(data,0,sizeof(data));assert(fread(data,1,12,roots)==12&&!strcmp(data,"trusted-test"));assert(!fclose(roots));
    roots=fopen("/etc/ssl/ca-bundle.pem","r");assert(roots);assert(!fclose(roots));
    const char *code=getenv("STEAMOHOS_CODE_ROOT"),*root=getenv("STEAMOHOS_STEAM_ROOT");
    assert(chdir("")==-1&&errno==ENOENT);assert(!chdir(code));
    char cwd[4096];assert(getcwd(cwd,sizeof(cwd))&&!strcmp(cwd,root));
    file=fopen("public/resource.txt","r");assert(file);assert(!fclose(file));
    char explicit_resource[4096];snprintf(explicit_resource,sizeof(explicit_resource),"%s/sub/../public/resource.txt",code);
    file=fopen(explicit_resource,"r");assert(file);assert(!fclose(file));
    snprintf(explicit_resource,sizeof(explicit_resource),"%s/.steam/root/public/resource.txt",getenv("HOME"));
    file=fopen(explicit_resource,"r");assert(file);assert(!fclose(file));
    assert(getenv("FONTCONFIG_FILE"));file=fopen(getenv("FONTCONFIG_FILE"),"r");assert(file);assert(!fclose(file));
    char pipe_path[4096];snprintf(pipe_path,sizeof(pipe_path),"%s/.steam/steam.pipe",getenv("HOME"));
    assert(!mkfifo(pipe_path,0600));assert(!stat(pipe_path,&info)&&S_ISFIFO(info.st_mode));
    int reader=open(pipe_path,O_RDONLY|O_NONBLOCK);assert(reader>=0);
    int writer=open(pipe_path,O_WRONLY|O_NONBLOCK);assert(writer>=0);assert(write(writer,"P",1)==1);
    char token;assert(read(reader,&token,1)==1&&token=='P');assert(!close(reader));assert(!close(writer));assert(!unlink(pipe_path));
    // Chromium's direct /dev/shm files and POSIX shm_open share one namespace.
    // mkostemp must preserve the caller's small logical buffer and CLOEXEC.
    assert(!access("/dev/shm",W_OK|X_OK));
    assert(!stat("/dev/shm",&info)&&S_ISDIR(info.st_mode)&&info.st_uid==geteuid());
    char chromium[]="/dev/shm/.com.valvesoftware.Steam.XXXXXX";
    int chrome=mkostemp(chromium,O_CLOEXEC);assert(chrome>=0);
    assert(!strncmp(chromium,"/dev/shm/.com.valvesoftware.Steam.",32));
    assert(fcntl(chrome,F_GETFD)&FD_CLOEXEC);
    assert(!ftruncate(chrome,4096));
    char *chrome_map=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,chrome,0);assert(chrome_map!=MAP_FAILED);
    strcpy(chrome_map,"direct-file-parent");
    pid_t chrome_child=fork();assert(chrome_child>=0);
    if(!chrome_child){
        int same=shm_open(chromium+8,O_RDWR,0);assert(same>=0);
        char *view=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,same,0);assert(view!=MAP_FAILED);
        assert(!strcmp(view,"direct-file-parent"));strcpy(view,"posix-child");
        assert(!munmap(view,4096));assert(!close(same));_exit(0);
    }
    int chrome_status;assert(waitpid(chrome_child,&chrome_status,0)==chrome_child&&WIFEXITED(chrome_status)&&!WEXITSTATUS(chrome_status));
    assert(!strcmp(chrome_map,"posix-child"));
    assert(!unlink(chromium));assert(open(chromium,O_RDONLY)==-1&&errno==ENOENT);
    assert(shm_open(chromium+8,O_RDONLY,0)==-1&&errno==ENOENT);
    strcpy(chrome_map,"unlinked-and-shared");assert(!strcmp(chrome_map,"unlinked-and-shared"));
    assert(!munmap(chrome_map,4096));assert(!close(chrome));
    puts("PASS direct /dev/shm and POSIX namespace, mkostemp CLOEXEC, fork mapping and unlink lifetime");
    const char *shm_name="/steamohos-filesystem-test";
    (void)shm_unlink(shm_name);
    int shm=shm_open(shm_name,O_RDWR|O_CREAT|O_EXCL,0600);assert(shm>=0);
    assert(fcntl(shm,F_GETFD)&FD_CLOEXEC);
    assert(shm_open(shm_name,O_RDWR|O_CREAT|O_EXCL,0600)==-1&&errno==EEXIST);
    assert(!ftruncate(shm,4096));
    char *shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,shm,0);assert(shared!=MAP_FAILED);
    memcpy(shared,"parent",7);
    pid_t child=fork();assert(child>=0);
    if(!child){
        int other=shm_open(shm_name,O_RDWR,0);assert(other>=0);
        char *view=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,other,0);assert(view!=MAP_FAILED);
        assert(!strcmp(view,"parent"));strcpy(view,"child");assert(!munmap(view,4096));close(other);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    assert(!strcmp(shared,"child"));assert(!shm_unlink(shm_name));
    assert(shm_open(shm_name,O_RDWR,0)==-1&&errno==ENOENT);
    strcpy(shared,"still mapped");assert(!strcmp(shared,"still mapped"));
    assert(!munmap(shared,4096));assert(!close(shm));
    for(int i=0;i<4;++i){const char *bad[]={"/","/../escape","/a/b","/.."};
        assert(shm_open(bad[i],O_CREAT|O_RDWR,0600)==-1&&errno==EINVAL);}
    assert(shm_unlink(shm_name)==-1&&errno==ENOENT);
    puts("PASS POSIX shm exclusive create, CLOEXEC, fork visibility, unlink while mapped and errors");
    puts("PASS real private /tmp mkdir/stat/open/fopen/rename/unlink, ownership and errno; unrelated paths; GNU executable readlink and truncation; code/data resource chdir; real private FIFO IO");
}
''')
            runner = base / "runner"
            subprocess.run(["cc", str(source), "-o", str(runner)], check=True)
            library = base / "compat.so"
            fixture = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"
            subprocess.run(["cc", "-O2", "-shared", "-fPIC", str(fixture / "posix_compat.c"),
                            str(fixture / "sysvipc_compat.c"), str(fixture / "filesystem_compat.c"), str(fixture / "syscall_compat.S"),
                            "-pthread", "-ldl", "-o", str(library)], check=True)
            executable = base / "signed-entry"
            executable.write_bytes(runner.read_bytes())
            bundle = base / "trusted-roots.crt"
            bundle.write_text("trusted-test")
            code = base / "code"
            data_root = base / "data"
            for directory in [code / "sub", data_root / "sub", data_root / "public", base / "home/.steam", base / "ipc"]:
                directory.mkdir(parents=True, exist_ok=True)
            (base / "ipc").chmod(0o700)
            (data_root / "public/resource.txt").write_text("external-resource")
            environment = os.environ.copy()
            environment.update(LD_PRELOAD=str(library), STEAMOHOS_TMP_ROOT=str(private),
                               STEAMOHOS_EXEC_PATH=str(executable), SSL_CERT_FILE=str(bundle),
                               STEAMOHOS_CODE_ROOT=str(code), STEAMOHOS_STEAM_ROOT=str(data_root),
                               HOME=str(base / "home"), STEAMOHOS_IPC_ROOT=str(base / "ipc"))
            unrelated = base / "unrelated-output"
            result = subprocess.run([str(runner), str(executable), str(unrelated)], env=environment,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((private / "steamohos-logical-output").read_text(), "private-output")
            self.assertEqual(unrelated.read_text(), "unrelated-output")
            print(result.stdout, end="")
            robust = base / "robust"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(ROOT / "host_tests/linux_steam_sysvipc_test.c"), "-o", str(robust)],check=True)
            result=subprocess.run([str(robust)],env=environment,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("real kernel owner death after SIGKILL",result.stdout)
            print(result.stdout,end="")
            sqlite_environment = environment.copy()
            sqlite_environment.pop("STEAMOHOS_EXEC_PATH", None)
            sqlite_probe = r'''
import os, sqlite3
directory=os.environ['HOME']+'/.pki/nssdb'
os.makedirs(directory)
database=directory+'/cert9.db'
connection=sqlite3.connect(database, timeout=0.05)
connection.execute('create table certificates (value text)')
connection.execute("insert into certificates values ('persistent-test')")
connection.commit()
connection.execute('begin immediate')
child=os.fork()
if not child:
    contender=sqlite3.connect(database,timeout=0.05)
    try:
        contender.execute('begin immediate')
    except sqlite3.OperationalError as error:
        assert 'locked' in str(error)
        os._exit(0)
    os._exit(1)
assert os.waitpid(child,0)[1]==0
connection.rollback()
connection.close()
with sqlite3.connect(database) as reopened:
    assert reopened.execute('select value from certificates').fetchone()==('persistent-test',)
print('PASS private NSS database persistence and real SQLite cross-process exclusion')
'''
            result = subprocess.run([sys.executable, "-c", sqlite_probe], env=sqlite_environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((base / "ipc/pki/nssdb/cert9.db").is_file())
            self.assertFalse((base / "home/.pki").exists())
            print(result.stdout, end="")

if __name__ == "__main__":
    unittest.main()
