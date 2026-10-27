"""Real rtld/fork/dirfd tests for native Steam FEX routing, without CPU emulation.

The small host FEX stand-in tests launch contracts, NOT x86 execution or GPU.
It has no execute bit, starts a sibling FEXServer and issues syscall(openat).
"""
from pathlib import Path
import json, os, re, shutil, signal, subprocess, tempfile, time, unittest
ROOT=Path(__file__).resolve().parents[1]
NATIVE=ROOT/'entry/src/main/cpp/linux_steam'
class NativeFex(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='steamohos-native-fex-',dir='/var/tmp')
  cls.base=Path(cls.tmp.name);cls.code=cls.base/'signed';cls.code.mkdir()
  cls.library=cls.base/'library';cls.common=cls.library/'steamapps/common';cls.common.mkdir(parents=True)
  cls.tool=cls.common/'FEX-Emu';(cls.tool/'usr/bin').mkdir(parents=True)
  for name in ['fex-compat-tool','FEXCompatTool']:(cls.tool/name).write_text('never executed Python or depot wrapper\n')
  cls.runtime=cls.common/'SteamLinuxRuntime_soldier';cls.runtime.mkdir()
  (cls.runtime/'_v2-entry-point').write_text('never executed pressure-vessel\n')
  cls.scout=cls.common/'SteamLinuxRuntime';cls.scout.mkdir()
  (cls.scout/'scout-on-soldier-entry-point-v2').write_text('never executed scout wrapper\n')
  cls.rootfs=cls.runtime/'soldier_platform_1/files'
  (cls.rootfs/'usr/lib/x86_64-linux-gnu').mkdir(parents=True)
  (cls.rootfs/'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2').write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  (cls.rootfs/'usr/lib/data').write_text('guest-data')
  (cls.rootfs/'lib').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/lib')
  # A real OHOS emulated symlink record, not a host symlink.
  (cls.rootfs/'absolute-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/lib')
  (cls.rootfs/'escape-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/../../outside')
  cls.game=cls.common/'Generic Game';cls.game.mkdir();cls.binary=cls.game/'game.x86_64'
  cls.binary.write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  cls.compat=cls.library/'steamapps/compatdata/2923300'
  (cls.base/'ipc').mkdir()
  obj=[]
  for name in ['posix_compat.c','filesystem_compat.c','sysvipc_compat.c','socket_metadata.c','popen_compat.c','exec_compat.cpp','syscall_compat.S']:
   source=NATIVE/'gnu_fixture'/name;target=cls.base/(name+'.o');obj.append(str(target))
   subprocess.run(['g++' if name.endswith('.cpp') else 'cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',str(source),'-o',str(target)],check=True)
  for name in ['native_loader.cpp','native_loader_entry.S']:
   target=cls.base/(name+'.o');obj.append(str(target))
   subprocess.run(['g++','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',str(NATIVE/name),'-o',str(target)],check=True)
  subprocess.run(['g++','-shared',*obj,'-ldl','-pthread','-o',str(cls.code/'libsteamohos_posix.so')],check=True)
  subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(NATIVE/'gnu_fixture/steamohos_fex.cpp'),'-o',str(cls.code/'steamohos_fex')],check=True)
  caller=cls.base/'caller.c';caller.write_text('#include <unistd.h>\n#include <stdio.h>\nint main(int n,char**v){if(n<2)return 64;execv(v[1],v+1);perror("caller exec");return 77;}')
  subprocess.run(['cc',str(caller),'-o',str(cls.base/'caller')],check=True)
  source=cls.base/'fex.c';source.write_text(r"""
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int n,char**v) {
 if(n==3&&!strcmp(v[1],"--wait_pipe")){unsigned long long ok=0;assert(write(atoi(v[2]),&ok,8)==8);return 0;}
 assert(n>=2);assert(!strcmp(getenv("SteamAppId"),"2923300"));
 assert(getenv("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET"));
 assert(!strcmp(getenv("STEAM_COMPAT_DATA_PATH"),getenv("STEAMOHOS_NATIVE_FEX_IPC")));
 assert(strstr(getenv("STEAM_COMPAT_DATA_PATH"),"/ipc/f"));
 assert(strlen(getenv("STEAM_COMPAT_DATA_PATH"))+strlen("/fex-emu/Server/4294967295.FEXServer.Socket")<108);
 assert(!getenv("STEAMOHOS_SOFTWARE_GL"));assert(!getenv("LIBGL_ALWAYS_SOFTWARE"));
 char path[4096],data[16]={0};snprintf(path,sizeof(path),"%s/absolute-link",getenv("FEX_ROOTFS"));
 int root=open(path,O_RDONLY|O_DIRECTORY);assert(root>=0);
 int fd=syscall(SYS_openat,root,"data",O_RDONLY,0);assert(fd>=0);
 assert(read(fd,data,10)==10&&!strcmp(data,"guest-data"));close(fd);close(root);
 snprintf(path,sizeof(path),"%s/escape-link",getenv("FEX_ROOTFS"));
 assert(syscall(SYS_openat,AT_FDCWD,path,O_RDONLY,0)<0&&errno==EACCES);
 int pipes[2];assert(!pipe(pipes));pid_t child=fork();assert(child>=0);
 if(!child){close(pipes[0]);char server[4096],number[20];snprintf(server,sizeof(server),"%s/usr/bin/FEXServer",getenv("STEAMOHOS_NATIVE_FEX_TOOL"));snprintf(number,sizeof(number),"%d",pipes[1]);execl(server,server,"--wait_pipe",number,NULL);_exit(87);}
 close(pipes[1]);unsigned long long ready=99;assert(read(pipes[0],&ready,8)==8&&ready==0);close(pipes[0]);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 puts("FEX_SERVER_REAL_HANDSHAKE_OK");
 printf("CONFIG=%s/Config.json\n",getenv("FEX_APP_CONFIG_LOCATION"));
 fflush(stdout);
 if(n>2&&!strcmp(v[2],"hold")){for(;;)pause();}
 assert(n==4&&!strcmp(v[2],"argument with spaces")&&!strcmp(v[3],"literal;$not-a-command"));
 fd=open(getenv("FEX_APP_CONFIG"),O_RDONLY);assert(fd>=0);close(fd);
 return 23;
}
""")
  subprocess.run(['cc',str(source),'-o',str(cls.tool/'usr/bin/FEX')],check=True)
  shutil.copyfile(cls.tool/'usr/bin/FEX',cls.tool/'usr/bin/FEXServer')
  for f in (cls.tool/'usr/bin').iterdir():f.chmod(0o660)
  interp=re.search(r'interpreter: (.*?)\]',subprocess.check_output(['readelf','-l',str(cls.base/'caller')],text=True)).group(1)
  shutil.copyfile(interp,cls.code/Path(interp).name)
  for name in ['libc.so.6','libstdc++.so.6','libgcc_s.so.1','libm.so.6']:
   shutil.copyfile(subprocess.check_output(['g++','-print-file-name='+name],text=True).strip(),cls.code/name)
  cls.env={**os.environ,'LD_PRELOAD':str(cls.code/'libsteamohos_posix.so'),'STEAMOHOS_CODE_ROOT':str(cls.code),
   'STEAMOHOS_LIBRARY_ROOT':str(cls.library),'STEAMOHOS_IPC_ROOT':str(cls.base/'ipc'),'STEAM_COMPAT_DATA_PATH':str(cls.compat),
   'STEAM_COMPAT_INSTALL_PATH':str(cls.game),'SteamAppId':'2923300','STEAMOHOS_SOFTWARE_GL':'1','LIBGL_ALWAYS_SOFTWARE':'1',
   'STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET':str(cls.base/'audio'),'GLIBC_TUNABLES':'glibc.pthread.rseq=0'}
  cls.env.pop('STEAMOHOS_NATIVE_FEX',None)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def command(self,*extra):
  return [str(self.base/'caller'),str(self.tool/'fex-compat-tool'),'waitforexitandrun','--',str(self.runtime/'_v2-entry-point'),'--verb=waitforexitandrun','--',str(self.scout/'scout-on-soldier-entry-point-v2'),str(self.binary),*extra]
 def run_command(self,args=None,env=None):
  return subprocess.run(args or self.command('argument with spaces','literal;$not-a-command'),env=env or self.env,capture_output=True,text=True,timeout=15)
 def test_real_loader_server_and_exit(self):
  result=self.run_command();self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  self.assertIn('FEX_SERVER_REAL_HANDSHAKE_OK',result.stdout)
  self.assertIn('status=exited code=23',result.stderr)
  self.assertFalse(list((self.compat/'fex-emu').glob('hmosdeck-*')))
 def test_compatdata_cannot_escape_library(self):
  result=self.run_command(env={**self.env,'STEAM_COMPAT_DATA_PATH':str(self.base/'userdata')})
  self.assertEqual(result.returncode,126);self.assertIn('compatdata_outside_library',result.stderr)
  self.assertFalse((self.base/'userdata').exists())
 def test_unknown_runtime_is_not_silently_stripped(self):
  command=self.command();command[5]='--verb=unknown'
  result=self.run_command(command);self.assertEqual(result.returncode,126);self.assertIn('runtime_verb_missing',result.stderr)
 def test_missing_server_reports_failure(self):
  server=self.tool/'usr/bin/FEXServer';saved=server.with_suffix('.saved');server.rename(saved)
  try:
   result=self.run_command();self.assertEqual(result.returncode,126);self.assertIn('fex_server_elf_missing',result.stderr)
  finally:saved.rename(server)
 def test_external_script_and_helper_without_capability_are_rejected(self):
  for name in ['usr/bin/FEX','other-script']:
   path=self.tool/name
   if not path.exists():path.write_text('#!/bin/sh\nexit 0\n')
   result=self.run_command([str(self.base/'caller'),str(path)])
   self.assertEqual(result.returncode,77,result.stdout+result.stderr)
 def test_cancel_preserves_configuration_and_signal_status(self):
  process=subprocess.Popen(self.command('hold'),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   import select
   deadline=time.monotonic()+10;lines=[]
   # Read a byte at a time: TextIO buffering must not hide the second line.
   content=b''
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    ready,_,_=select.select([process.stdout],[],[],1)
    if ready:
     block=os.read(process.stdout.fileno(),1)
     self.assertTrue(block);content+=block
   path=Path(content.decode().split('CONFIG=',1)[1].strip());document=json.loads(path.read_text())
   self.assertEqual(document['Config']['RootFS'],str(self.rootfs))
   self.assertIn('LD_PRELOAD=',document['Config']['Env'])
   self.assertIn('STEAM_COMPAT_DATA_PATH='+str(self.compat),document['Config']['Env'])
   process.send_signal(signal.SIGTERM)
   out,err=process.communicate(timeout=10)
   self.assertEqual(process.returncode,128+signal.SIGTERM,content.decode()+out+err)
   self.assertFalse(path.parent.exists())
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=5)
 def test_steam_appinfo_and_user_fex_settings_survive(self):
  process=subprocess.Popen(self.command('hold'),env={**self.env,'STEAM_FEX_TSOENABLED':'0',
   'STEAM_FEX_MULTIBLOCK':'1','STEAM_COMPAT_FEX_CONFIG':'TSOEnabled:1 Multiblock:0 ThunksDB_Vulkan:1'},
   stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   import select
   content=b'';deadline=time.monotonic()+10
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    if select.select([process.stdout],[],[],1)[0]:
     block=os.read(process.stdout.fileno(),1);self.assertTrue(block);content+=block
   config=Path(content.decode().split('CONFIG=',1)[1].strip())
   data=json.loads((config.parent/'app.json').read_text())
   self.assertEqual(data['Config'],dict(TSOEnabled='1',Multiblock='0'))
   self.assertEqual(data['ThunksDB'],dict(GL='0'))
   # An absent Vulkan thunk remains disabled in the base config despite ':1'.
   self.assertEqual(json.loads(config.read_text())['ThunksDB']['Vulkan'],'0')
   process.send_signal(signal.SIGTERM);process.communicate(timeout=10)
   self.assertFalse(config.parent.exists())
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=5)
if __name__=='__main__':unittest.main()
