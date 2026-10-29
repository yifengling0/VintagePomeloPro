"""Real libudev enumeration plus sandbox-denied monitor/ref/poll semantics."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
C=r"""
#define _GNU_SOURCE
#include <libudev.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>
static void deny(int error) {
 struct sock_filter f[]={
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_socket,0,3),
 BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AF_NETLINK,0,1),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|error),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog p={sizeof(f)/sizeof(*f),f};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
 assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&p));
}
int main(int n,char**v) {
 assert(n==2);struct udev *u=udev_new();assert(u);
 int marker=73;udev_set_userdata(u,&marker);assert(udev_get_userdata(u)==&marker);
 struct udev_enumerate *e=udev_enumerate_new(u);assert(e);
 assert(!udev_enumerate_add_match_subsystem(e,"net"));assert(!udev_enumerate_scan_devices(e));
 unsigned count=0;struct udev_list_entry *item;
 udev_list_entry_foreach(item,udev_enumerate_get_list_entry(e)) {
  const char *name=udev_list_entry_get_name(item);assert(name&&name[0]=='/');
  struct udev_device *d=udev_device_new_from_syspath(u,name);assert(d);
  assert(!strcmp(udev_device_get_subsystem(d),"net"));udev_device_unref(d);count++;
 }
 assert(count>0);udev_enumerate_unref(e);
 if(!strcmp(v[1],"real")) {
  struct udev_monitor *m=udev_monitor_new_from_netlink(u,"udev");assert(m);
  int fd=udev_monitor_get_fd(m);assert(fd>=0&&fcntl(fd,F_GETFD)>=0);
  assert(!udev_monitor_filter_add_match_subsystem_devtype(m,"input",NULL));
  assert(!udev_monitor_enable_receiving(m));udev_monitor_ref(m);
  assert(!udev_monitor_unref(m));assert(fcntl(fd,F_GETFD)>=0);
  assert(!udev_monitor_unref(m));assert(fcntl(fd,F_GETFD)<0&&errno==EBADF);
 } else {
  int error=!strcmp(v[1],"other")?EINVAL:EPERM;deny(error);
  struct udev_monitor *m=udev_monitor_new_from_netlink(u,"udev");
  if(strcmp(v[1],"quiet")) {assert(!m&&errno==error);}
  else {
   assert(m&&udev_monitor_get_udev(m)==u);int fd=udev_monitor_get_fd(m);
   assert(fd>=0&&(fcntl(fd,F_GETFD)&FD_CLOEXEC));assert(fcntl(fd,F_GETFL)&O_NONBLOCK);
   assert(!udev_monitor_filter_add_match_subsystem_devtype(m,"input",NULL));
   assert(!udev_monitor_enable_receiving(m));struct pollfd p={fd,POLLIN,0};
   assert(!poll(&p,1,20));assert(!udev_monitor_receive_device(m)&&errno==EAGAIN);
   assert(udev_monitor_ref(m)==m);assert(!udev_monitor_unref(m));assert(fcntl(fd,F_GETFD)>=0);
   assert(!udev_monitor_unref(m));assert(fcntl(fd,F_GETFD)<0&&errno==EBADF);
   struct udev_monitor *all[32];for(int i=0;i<32;i++){all[i]=udev_monitor_new_from_netlink(u,"kernel");assert(all[i]);}
   assert(!udev_monitor_new_from_netlink(u,"udev")&&errno==EMFILE);
   for(int i=0;i<32;i++)assert(!udev_monitor_unref(all[i]));
   m=udev_monitor_new_from_netlink(u,"udev");assert(m);assert(!udev_monitor_unref(m));
  }
 }
 assert(!udev_unref(u));return 0;
}
"""
class UdevAdapter(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='native-udev-');cls.root=Path(cls.tmp.name)
  s=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/native_udev.c'
  subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(s),'-ldl','-pthread','-o',str(cls.root/'libudev.so.1')],check=True)
  (cls.root/'test.c').write_text(C)
  subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(cls.root/'test.c'),'-L'+str(cls.root),'-l:libudev.so.1','-Wl,-rpath,'+str(cls.root),'-o',str(cls.root/'test')],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def run_mode(self,mode,active):
  env={**os.environ,'STEAMOHOS_NATIVE_FEX':active}
  r=subprocess.run([str(self.root/'test'),mode],env=env,capture_output=True,text=True,timeout=10)
  self.assertEqual(r.returncode,0,r.stderr)
 def test_real_monitor_and_enumeration_forwarding(self):self.run_mode('real','1')
 def test_permission_denial_quiet_poll_and_lifecycle(self):self.run_mode('quiet','1')
 def test_outside_native_scope_preserves_permission_failure(self):self.run_mode('denied','0')
 def test_unrelated_error_is_not_hidden(self):self.run_mode('other','1')
if __name__=='__main__':unittest.main()
