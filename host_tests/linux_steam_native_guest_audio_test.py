"""Guest PCM module load and metadata-denied shared-ring length contract."""
import os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class GuestAudio(unittest.TestCase):
 def test_ring_length_without_fstat_and_unchanged_offset(self):
  with tempfile.TemporaryDirectory(prefix='guest-alsa-') as name:
   root=Path(name);source=root/'test.c';binary=root/'test'
   source.write_text('#include "'+str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/native_guest_audio.c')+'"\n'+r"""
#include <assert.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
int main(void) {
 int fd=memfd_create("private-audio-ring",MFD_CLOEXEC);assert(fd>=0);
 assert(!ftruncate(fd,32840));assert(lseek(fd,17,SEEK_SET)==17);
 struct sock_filter f[]={
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_newfstatat,1,0),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_fstat,0,1),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EFAULT),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog p={sizeof(f)/sizeof(*f),f};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p));
 struct stat status;assert(fstat(fd,&status)==-1&&errno==EFAULT);
 assert(!native_guest_ring_stat(fd,&status));assert(status.st_size==32840);
 assert(lseek(fd,0,SEEK_CUR)==17);close(fd);
 assert(native_guest_ring_stat(-1,&status)==-1);
 return 0;
}
""")
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DPIC',str(source),'-lasound','-o',str(binary)],check=True)
   result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
   self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':unittest.main()
