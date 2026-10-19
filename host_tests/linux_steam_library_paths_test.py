"""Exercise the activated single-library namespace through real GNU file IO."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class LibraryPaths(unittest.TestCase):
    def test_single_library_alias(self):
        with tempfile.TemporaryDirectory(prefix='steam-library-', dir='/var/tmp') as temporary:
            base = Path(temporary)
            library, client, code, home = [base / x for x in ('library', 'client', 'code', 'home')]
            for root in (library, client, code, home / '.steam/root'):
                (root / 'steamapps').mkdir(parents=True)
            (client / 'config').mkdir()
            (client / 'config/original').write_bytes(b'client-config')
            (library / 'steamapps/appmanifest_695630.acf').write_bytes(b'game-manifest')
            (client / 'steamapps/physical').write_bytes(b'client-original')
            source = ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture/filesystem_compat.c'
            compat = base / 'compat.so'
            subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', str(source), '-ldl', '-o', str(compat)], check=True)
            script = r"""
import ctypes,os,resource,signal
from pathlib import Path
library,client,code,home=[Path(os.environ[k]) for k in ['STEAMOHOS_LIBRARY_ROOT','STEAMOHOS_STEAM_ROOT','STEAMOHOS_CODE_ROOT','HOME']]
api=ctypes.CDLL(None);api.realpath.argtypes=[ctypes.c_char_p,ctypes.c_void_p];api.realpath.restype=ctypes.c_void_p
def real(path):
 out=ctypes.create_string_buffer(4096);assert api.realpath(os.fsencode(path),out);return Path(os.fsdecode(out.value))
api.__realpath_chk.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t]
api.__realpath_chk.restype=ctypes.c_void_p
def checked_real(path):
 out=ctypes.create_string_buffer(4096)
 assert api.__realpath_chk(os.fsencode(path),out,len(out))
 return Path(os.fsdecode(out.value))
class Dirent(ctypes.Structure):
 _fields_=[('ino',ctypes.c_ulong),('offset',ctypes.c_long),('length',ctypes.c_ushort),('type',ctypes.c_ubyte),('name',ctypes.c_char*256)]
api.free.argtypes=[ctypes.c_void_p]
def scan(path,wide=False,fd=None):
 entries=ctypes.POINTER(ctypes.POINTER(Dirent))()
 name=('scandirat' if fd is not None else 'scandir')+('64' if wide else '')
 fn=getattr(api,name);args=[os.fsencode(path),ctypes.byref(entries),None,None]
 if fd is not None:args.insert(0,fd)
 n=fn(*args);assert n>=0,(name,ctypes.get_errno());names=[]
 for i in range(n):names.append(os.fsdecode(entries[i].contents.name));api.free(entries[i])
 api.free(entries);return names
active=os.environ['ACTIVE']=='1'
assert (client/'config/original').read_bytes()==b'client-config'
if not active:
 assert (client/'steamapps/physical').read_bytes()==b'client-original'
 assert not (client/'steamapps/appmanifest_695630.acf').exists()
else:
 for root in (client,code,home/'.steam/root'):
  apps=root/'steamapps'
  assert (apps/'appmanifest_695630.acf').read_bytes()==b'game-manifest'
  assert real(apps/'appmanifest_695630.acf')==library/'steamapps/appmanifest_695630.acf'
  assert checked_real(apps/'appmanifest_695630.acf')==library/'steamapps/appmanifest_695630.acf'
  # The libc checked variant must still reject an undersized destination.
  child=os.fork()
  if child==0:
   resource.setrlimit(resource.RLIMIT_CORE,(0,0))
   os.close(2);small=ctypes.create_string_buffer(2)
   api.__realpath_chk(os.fsencode(apps/'appmanifest_695630.acf'),small,2)
   os._exit(1)
  _,status=os.waitpid(child,0)
  assert os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGABRT
  out=ctypes.create_string_buffer(4096)
  assert not api.__realpath_chk(os.fsencode(apps/'missing'),out,len(out))
  assert not (apps/'physical').exists()
  fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
  for wide in (False,True):
   assert 'appmanifest_695630.acf' in scan(apps,wide)
   assert 'appmanifest_695630.acf' in scan('steamapps',wide,fd)
  leaf='steamapps/new.dat'
  file=os.open(leaf,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600,dir_fd=fd);os.write(file,b'new');os.close(file)
  assert (library/'steamapps/new.dat').read_bytes()==b'new'
  os.rename(leaf,leaf+'.renamed',src_dir_fd=fd,dst_dir_fd=fd)
  assert (library/'steamapps/new.dat.renamed').read_bytes()==b'new'
  os.unlink(leaf+'.renamed',dir_fd=fd);os.close(fd)
  assert 'appmanifest_695630.acf' in os.listdir(apps)
  previous=os.getcwd();os.chdir(root)
  assert Path('steamapps/appmanifest_695630.acf').read_bytes()==b'game-manifest'
  os.chdir(previous)
 assert not (client/'steamapps-other/appmanifest_695630.acf').exists()
 assert not (client/'steamapps/../../escape').exists()
 assert not (client.parent/'WindowsSteam/steamapps/appmanifest_695630.acf').exists()
print('PASS single-library native IO, realpath, dirfd, relative paths and scope; active='+str(active))
"""
            for active in (False, True):
                if active:
                    (library / 'steamapps/.hmosdeck-single-library-v2').write_text(str(library))
                env = {**os.environ, 'LD_PRELOAD': str(compat), 'ACTIVE': str(int(active)),
                    'STEAMOHOS_LIBRARY_ROOT': str(library), 'STEAMOHOS_STEAM_ROOT': str(client),
                    'STEAMOHOS_CODE_ROOT': str(code), 'HOME': str(home)}
                result = subprocess.run([sys.executable, '-c', script], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertEqual((client/'steamapps/physical').read_bytes(), b'client-original')
                print(result.stdout, end='')
if __name__ == '__main__':
    unittest.main()
