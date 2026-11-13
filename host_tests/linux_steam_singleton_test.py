"""Exercise Chromium singleton exclusion and real Unix IPC in the GNU preload."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

RUNNER = r'''#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_ok(pid_t child) {
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static struct sockaddr_un endpoint(const char *path) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    assert(strlen(path)<sizeof(address.sun_path));strcpy(address.sun_path,path);return address;
}
int main(void) {
    char profile[4096],lock[4096],cookie[4096],link[4096];
    snprintf(profile,sizeof(profile),"%s/config/htmlcache",getenv("STEAMOHOS_STEAM_ROOT"));
    assert(strlen(profile)+sizeof("/SingletonCookie")<sizeof(lock));
    strcpy(lock,profile);strcat(lock,"/SingletonLock");
    strcpy(cookie,profile);strcat(cookie,"/SingletonCookie");
    struct stat st;
    assert(!stat(profile,&st)&&S_ISDIR(st.st_mode));
    assert(!symlink("host-12345",lock));
    assert(symlink("host-99999",lock)==-1&&errno==EEXIST);
    memset(link,0,sizeof(link));
    assert(readlink(lock,link,sizeof(link))==10&&!strcmp(link,"host-12345"));
    assert(readlink(lock,link,4)==4&&!memcmp(link,"host",4));
    assert(readlink(lock,link,0)==-1&&errno==EINVAL);
    assert(!lstat(lock,&st)&&S_ISLNK(st.st_mode)&&st.st_size==10);
    assert(stat(lock,&st)==-1&&errno==ENOENT);
    assert(open(lock,O_RDONLY|O_NOFOLLOW)==-1&&errno==ELOOP);
    assert(open(lock,O_CREAT|O_EXCL|O_WRONLY,0600)==-1&&errno==EEXIST);
    int directory=open(profile,O_RDONLY|O_DIRECTORY);assert(directory>=0);
    assert(!fstatat(directory,"SingletonLock",&st,AT_SYMLINK_NOFOLLOW)&&S_ISLNK(st.st_mode));
    assert(fstatat(directory,"SingletonLock",&st,0)==-1&&errno==ENOENT);
    assert(readlinkat(directory,"SingletonLock",link,4)==4&&!memcmp(link,"host",4));
    assert(!symlinkat("token123",directory,"SingletonCookie"));
    assert(readlinkat(directory,"SingletonCookie",link,8)==8&&!memcmp(link,"token123",8));
    int fd=openat(directory,"token123",O_CREAT|O_WRONLY|O_EXCL,0600);assert(fd>=0);
    assert(write(fd,"cookie",6)==6);close(fd);
    fd=openat(directory,"SingletonCookie",O_RDONLY);assert(fd>=0);
    assert(read(fd,link,6)==6&&!memcmp(link,"cookie",6));close(fd);
    assert(!fstatat(directory,"SingletonCookie",&st,0)&&S_ISREG(st.st_mode)&&st.st_size==6);
    assert(!unlinkat(directory,"SingletonCookie",0));
    assert(!unlinkat(directory,"token123",0));
    assert(!unlink(lock));
    assert(readlink(lock,link,sizeof(link))==-1&&errno==ENOENT);

    // Concurrent contenders must see either the complete winning target or EEXIST.
    for(int iteration=0;iteration<8;++iteration) {
        int start[2];assert(!pipe(start));pid_t children[2];
        for(int i=0;i<2;++i){children[i]=fork();assert(children[i]>=0);
            if(!children[i]){close(start[1]);char go;assert(read(start[0],&go,1)==1);
                if(!symlink(i?"second-5678":"first-1234",lock))_exit(20);
                assert(errno==EEXIST);assert(readlink(lock,link,sizeof(link))>0);_exit(21);}}
        close(start[0]);assert(write(start[1],"!!",2)==2);close(start[1]);
        int won=0,lost=0;for(int i=0;i<2;++i){int status;assert(waitpid(children[i],&status,0)==children[i]);
            assert(WIFEXITED(status));won+=WEXITSTATUS(status)==20;lost+=WEXITSTATUS(status)==21;}
        assert(won==1&&lost==1);assert(!unlink(lock));
    }

    char temporary[]="/tmp/cef-singleton-XXXXXX";assert(mkdtemp(temporary));
    char socketpath[108],tmpcookie[256],socketlink[4096];
    snprintf(socketpath,sizeof(socketpath),"%s/SingletonSocket",temporary);
    snprintf(tmpcookie,sizeof(tmpcookie),"%s/SingletonCookie",temporary);
    assert(!symlink("cookie987",tmpcookie));
    assert(readlink(tmpcookie,link,9)==9&&!memcmp(link,"cookie987",9));
    strcpy(socketlink,profile);strcat(socketlink,"/SingletonSocket");
    assert(symlink("/etc/not-private",socketlink)==-1&&errno==ENOTSUP);
    assert(!symlink(socketpath,socketlink));
    int server=socket(AF_UNIX,SOCK_STREAM,0);assert(server>=0);
    struct sockaddr_un address=endpoint(socketpath);
    assert(!bind(server,(struct sockaddr*)&address,sizeof(address)));assert(!listen(server,1));
    pid_t child=fork();assert(child>=0);
    if(!child){close(server);int client=socket(AF_UNIX,SOCK_STREAM,0);assert(client>=0);
        // The production profile path can exceed sun_path: use a dirfd/cwd path.
        assert(!chdir(profile));address=endpoint("SingletonSocket");
        assert(!connect(client,(struct sockaddr*)&address,sizeof(address)));
        assert(write(client,"hello",5)==5);assert(read(client,link,5)==5&&!memcmp(link,"reply",5));
        close(client);_exit(0);}
    int peer=accept(server,NULL,NULL);assert(peer>=0);
    assert(read(peer,link,5)==5&&!memcmp(link,"hello",5));assert(write(peer,"reply",5)==5);
    close(peer);close(server);child_ok(child);
    assert(!unlink(socketlink));assert(!unlink(socketpath));assert(!unlink(tmpcookie));assert(!rmdir(temporary));
    // Unreserved links remain genuine filesystem links.
    assert(!symlink("ordinary-target","/tmp/ordinary-link"));
    assert(!lstat("/tmp/ordinary-link",&st)&&S_ISLNK(st.st_mode));
    assert(!unlink("/tmp/ordinary-link"));close(directory);
    puts("PASS singleton lock races, profile isolation, cookie and dirfd semantics; real cross-process Unix socket request/reply");
}
'''


class Singleton(unittest.TestCase):
    def test_profile_exclusion_and_ipc(self):
        with tempfile.TemporaryDirectory(prefix="cef-", dir="/var/tmp") as temporary:
            base = Path(temporary)
            for name in ["tmp", "ipc", "data/config"]:
                (base / name).mkdir(mode=0o700, parents=True)
            source = base / "test.c"
            source.write_text(RUNNER)
            runner, library = base / "runner", base / "compat.so"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(runner)], check=True)
            fixture = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                            *[str(fixture / name) for name in ["filesystem_compat.c", "posix_compat.c", "syscall_compat.S"]],
                            "-ldl", "-pthread", "-o", str(library)], check=True)
            environment = os.environ.copy()
            environment.pop("FONTCONFIG_FILE", None)
            environment.update(LD_PRELOAD=str(library), STEAMOHOS_TMP_ROOT=str(base / "tmp"),
                               STEAMOHOS_IPC_ROOT=str(base / "ipc"), STEAMOHOS_STEAM_ROOT=str(base / "data"))
            result = subprocess.run([str(runner)], env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((base / "data/config/htmlcache").exists())
            self.assertTrue((base / "ipc/cef-profile").is_dir())
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
