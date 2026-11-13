"""Real depot staging/commit/verification/upgrade/removal with OHOS symlink denial."""
from pathlib import Path
import os, subprocess, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
SCRIPT=r"""
import ctypes, errno, fcntl, os, shutil, stat, subprocess, sys
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from pathlib import Path
base=Path(os.environ['TEST_BASE']);library=base/'library';apps=library/'steamapps'
stage=apps/'downloading/1070560';(stage/'steam-runtime/amd64').mkdir(parents=True)
(stage/'steam-runtime/etc').mkdir();(stage/'steam-runtime/etc/test.conf').write_bytes(b'real-content')
link=stage/'steam-runtime/amd64/etc'
def fails(call,wanted):
 try:call()
 except OSError as e:assert e.errno==wanted,(e,wanted)
 else:raise AssertionError('Expected errno '+str(wanted))
os.symlink('../etc',link)
assert stat.S_ISLNK(link.lstat().st_mode) and link.lstat().st_size==6
assert link.is_dir() and (link/'test.conf').read_bytes()==b'real-content'
assert os.readlink(link)=='../etc'
assert os.path.realpath(link)==str(stage/'steam-runtime/etc')
api=ctypes.CDLL(None,use_errno=True)
api.__realpath_chk.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_size_t]
buf=ctypes.create_string_buffer(4096)
assert api.__realpath_chk(os.fsencode(link),buf,len(buf)) and buf.value==os.fsencode(stage/'steam-runtime/etc')
api.readlink.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t]
assert api.readlink(os.fsencode(link),buf,3)==3 and buf.raw[:3]==b'../'
assert api.readlink(os.fsencode(link),buf,0)==-1 and ctypes.get_errno()==errno.EINVAL
# Actual statx ABI: mode offset 28, size offset 40, with original mask/errors.
api.statx.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_uint,ctypes.c_void_p]
assert api.statx(-100,os.fsencode(link),0x100,0x7ff,buf)==0
assert stat.S_ISLNK(int.from_bytes(buf.raw[28:30],sys.byteorder))
assert int.from_bytes(buf.raw[40:48],sys.byteorder)==6
assert api.statx(-100,os.fsencode(link),0,0x7ff,buf)==0
assert stat.S_ISDIR(int.from_bytes(buf.raw[28:30],sys.byteorder))
fails(lambda:os.open(link,os.O_RDONLY|os.O_NOFOLLOW),errno.ELOOP)
fails(lambda:os.open(link,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600),errno.EEXIST)
fails(lambda:os.symlink('../other',link),errno.EEXIST)
fails(lambda:os.mkdir(link),errno.EEXIST)
fails(lambda:os.rmdir(link),errno.ENOTDIR)
assert next(x for x in os.scandir(link.parent) if x.name=='etc').is_symlink()
# Symlinkat/readlinkat/fstatat/openat/renameat/unlinkat and parent links.
fd=os.open(link.parent,os.O_RDONLY|os.O_DIRECTORY)
os.symlink('etc', 'second',dir_fd=fd)
assert os.readlink('second',dir_fd=fd)=='etc'
assert stat.S_ISLNK(os.stat('second',dir_fd=fd,follow_symlinks=False).st_mode)
f=os.open('second/test.conf',os.O_RDONLY,dir_fd=fd);assert os.read(f,20)==b'real-content';os.close(f)
os.rename('second','renamed',src_dir_fd=fd,dst_dir_fd=fd);os.unlink('renamed',dir_fd=fd)
os.close(fd)
# Directory link/.. is resolved against target, not lexical link location.
(stage/'different/inner').mkdir(parents=True);(stage/'different/sibling').write_bytes(b'correct')
(stage/'steam-runtime/sibling').write_bytes(b'wrong')
os.symlink('../different/inner',stage/'steam-runtime/alias')
assert (stage/'steam-runtime/alias/../sibling').read_bytes()==b'correct'
os.symlink('later',stage/'dangling');assert os.readlink(stage/'dangling')=='later'
api.faccessat.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_int]
assert api.faccessat(-100,os.fsencode(stage/'dangling'),0,0x100)==0
assert api.faccessat(-100,os.fsencode(stage/'dangling'),0,0)==-1 and ctypes.get_errno()==errno.ENOENT
fails(lambda:(stage/'dangling').read_bytes(),errno.ENOENT)
(stage/'later').write_bytes(b'late');assert (stage/'dangling').read_bytes()==b'late'
os.symlink('b',stage/'a');os.symlink('a',stage/'b')
fails(lambda:(stage/'a').stat(),errno.ELOOP)
fails(lambda:os.path.realpath(stage/'a',strict=True),errno.ELOOP)
assert stat.S_ISLNK((stage/'a').lstat().st_mode)
# Store original target for depot validation, but reject escaping traversal.
outside=base/'user-save';outside.write_bytes(b'untouched-save')
os.symlink(str(outside),stage/'escape');assert os.readlink(stage/'escape')==str(outside)
fails(lambda:(stage/'escape').read_bytes(),errno.EACCES)
os.symlink('../../../../user-save',stage/'relative-escape')
fails(lambda:(stage/'relative-escape').read_bytes(),errno.EACCES)
# Malformed records remain regular files; no content is rewritten.
malformed=stage/'ordinary';malformed.write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n')
assert stat.S_ISREG(malformed.lstat().st_mode)
fails(lambda:os.readlink(malformed),errno.EINVAL)
# A reader seeing an empty in-progress record must wait for the writer guard,
# then inspect the completed bytes instead of caching a negative classification.
partial=os.open(stage/'publishing',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
guard=os.open(base/'ipc/depot-links.guard',os.O_RDWR)
fcntl.flock(guard,fcntl.LOCK_EX)
with ThreadPoolExecutor(max_workers=1) as pool:
 pending=pool.submit((stage/'publishing').lstat)
 try:
  try:pending.result(timeout=0.05)
  except TimeoutError:pass
  else:raise AssertionError('Reader did not wait for record publication')
 finally:
  os.write(partial,b'STEAMOHOS_DEPOT_SYMLINK_V1\nlater');os.close(partial)
  fcntl.flock(guard,fcntl.LOCK_UN);os.close(guard)
 assert stat.S_ISLNK(pending.result(timeout=5).st_mode)
# Directory scans must not re-walk every ancestor for every ordinary entry.
# Count real metadata calls instead of timing a fast host filesystem: increasing
# depth may add a fixed opening cost, not depth * entries document-store calls.
shallow=stage/'scan';deep=stage/'one/two/three/four/five/six/seven/scan'
for directory in [shallow,deep]:
 directory.mkdir(parents=True)
 for i in range(256):(directory/str(i)).write_bytes(b'ordinary-file-content-longer-than-the-record-magic')
counter=ctypes.CDLL(os.environ['TEST_DENY']);counter.depot_metadata_count.restype=ctypes.c_ulong
def scan_cost(directory):
 counter.depot_metadata_reset()
 assert len(list(os.scandir(directory)))==256
 return counter.depot_metadata_count()
short_cost=scan_cost(shallow);deep_cost=scan_cost(deep)
assert deep_cost-short_cost<100,(short_cost,deep_cost)
print('Directory scan metadata calls: shallow=%d, deep=%d for 256 entries'%(short_cost,deep_cost))
# Steady-state nofollow metadata/readlink must traverse a depot only once.
# Reuse the leaf metadata from the real syscall, without a persistent cache.
leaf=deep/'0';components=len(leaf.relative_to(stage).parts)
counter.depot_metadata_reset()
assert stat.S_ISREG(leaf.lstat().st_mode)
leaf_cost=counter.depot_metadata_count()
assert leaf_cost<=2,(leaf_cost,components)
counter.depot_metadata_reset()
fails(lambda:os.readlink(leaf),errno.EINVAL)
readlink_cost=counter.depot_metadata_count()
assert readlink_cost<=2,(readlink_cost,components)
# Physical success must not bypass the package boundary for '..', even when
# the kernel could leave and re-enter the same package and reach a real file.
fails(lambda:(stage/'../1070560/one/two/three/four/five/six/seven/scan/0').read_bytes(),errno.EACCES)
# Intermediate emulated links still need the component walker; no metadata or
# missing-path result is cached across replacement.
os.symlink('one',stage/'parent-alias')
assert (stage/'parent-alias/two/three/four/five/six/seven/scan/0').read_bytes()==leaf.read_bytes()
os.unlink(stage/'parent-alias');os.symlink('steam-runtime',stage/'parent-alias')
assert (stage/'parent-alias/etc/test.conf').read_bytes()==b'real-content'
os.unlink(stage/'parent-alias')
os.symlink('0',deep/'alias')
assert os.readlink(deep/'alias')=='0'
assert stat.S_ISLNK((deep/'alias').lstat().st_mode)
os.unlink(deep/'alias');os.symlink('1',deep/'alias')
assert os.readlink(deep/'alias')=='1'
print('Single-lookup metadata calls: lstat=%d, readlink=%d, components=%d'%
      (leaf_cost,readlink_cost,components))
# Rootfs-style probes retain the real parent descriptor instead of rescanning
# its deep absolute prefix, while a present emulated parent still resolves.
fd=os.open(deep,os.O_RDONLY|os.O_DIRECTORY)
for action in [lambda:os.stat('absent/one/two/three',dir_fd=fd,follow_symlinks=False),
               lambda:os.readlink('absent/one/two/three',dir_fd=fd)]:
 counter.depot_metadata_reset();fails(action,errno.ENOENT)
 assert counter.depot_metadata_count()<=1,counter.depot_metadata_count()
# The direct ENOENT must come from the original operation, including invalid
# flags/buffer precedence, rather than a fabricated result from a stat probe.
api.readlinkat.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t]
assert api.readlinkat(fd,b'absent/one',buf,0)==-1 and ctypes.get_errno()==errno.EINVAL
api.fstatat.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_int]
assert api.fstatat(fd,b'absent/one',buf,0x40000000)==-1 and ctypes.get_errno()==errno.EINVAL
os.symlink('missing-directory',deep/'virtual-parent')
fails(lambda:os.stat('virtual-parent/child',dir_fd=fd),errno.ENOENT)
os.unlink(deep/'virtual-parent');os.mkdir(deep/'missing-directory')
os.symlink('missing-directory',deep/'virtual-parent')
(deep/'missing-directory/child').write_bytes(b'visible')
assert os.stat('virtual-parent/child',dir_fd=fd).st_size==7
os.unlink(deep/'virtual-parent');os.unlink(deep/'missing-directory/child');os.rmdir(deep/'missing-directory');os.close(fd)
# An open directory remains usable after rename; record updates are visible
# without stale per-directory or negative lookup caches.
walking=stage/'walking';walking.mkdir();os.symlink('../later',walking/'link')
iterator=os.scandir(walking);os.rename(walking,stage/'walking-renamed')
assert next(iterator).is_symlink();iterator.close()
fd=os.open(stage/'walking-renamed',os.O_RDONLY|os.O_DIRECTORY)
with os.scandir(fd) as iterator:assert next(iterator).is_symlink()
os.close(fd)
# Reused stream pointers and descriptors must not retain another scope.
literal=base/'literal-directory';literal.mkdir()
(literal/'ordinary').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\nlater')
for _ in range(64):
 with os.scandir(stage/'walking-renamed') as iterator:assert next(iterator).is_symlink()
 with os.scandir(literal) as iterator:assert not next(iterator).is_symlink()
# Open stream state is safe to inherit across fork and close in another thread.
iterator=os.scandir(stage/'walking-renamed');child=os.fork()
if not child:
 try:ok=next(iterator).is_symlink();iterator.close();os._exit(0 if ok else 1)
 except BaseException:os._exit(2)
assert os.waitpid(child,0)[1]==0
with ThreadPoolExecutor(max_workers=1) as pool:pool.submit(iterator.close).result()
# Concurrent exclusive creators cannot publish partial records or overwrite.
def create(_):
 try:os.symlink('later',stage/'exclusive');return 1
 except FileExistsError:return 0
with ThreadPoolExecutor(max_workers=8) as pool:assert sum(pool.map(create,range(32)))==1
assert os.readlink(stage/'exclusive')=='later'
# No fake success for missing parents, or outside the configured depot tree.
fails(lambda:os.symlink('x',stage/'missing/link'),errno.ENOENT)
for relative in ['steamapps/compatdata','steamapps/userdata','steamapps/downloading/not-an-app','steamapps/temp/0','steamapps/common-other']:
 parent=library/relative;parent.mkdir(parents=True,exist_ok=True)
 fails(lambda:os.symlink('x',parent/'link'),errno.EPERM)
 assert not os.path.lexists(parent/'link')
# Commit: metadata stays with the depot, including alternate Steam folder names.
installed=apps/'common/SteamLinuxRuntime 1070560';installed.parent.mkdir()
os.rename(stage,installed)
link=installed/'steam-runtime/amd64/etc'
assert os.readlink(link)=='../etc' and (link/'test.conf').read_bytes()==b'real-content'
# Independently loaded client can verify links after restarting.
subprocess.run([sys.executable,'-c',"import os,sys;from pathlib import Path;p=Path(sys.argv[1]);assert os.readlink(p)=='../etc';assert (p/'test.conf').read_bytes()==b'real-content'",str(link)],check=True)
# Generic game depot, not a Runtime/AppID allowlist. Replace a link atomically,
# retaining each target and respecting renameat2 NOREPLACE semantics.
game=apps/'common/Any Linux Game';game.mkdir()
(game/'v1').write_bytes(b'one');(game/'v2').write_bytes(b'two')
os.symlink('v1',game/'lib.so');os.symlink('v2',game/'new-link')
api.renameat2.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]
assert api.renameat2(-100,os.fsencode(game/'new-link'),-100,os.fsencode(game/'lib.so'),1)==-1
assert ctypes.get_errno()==errno.EEXIST
os.replace(game/'new-link',game/'lib.so')
assert (game/'lib.so').read_bytes()==b'two' and (game/'v1').read_bytes()==b'one'
os.utime(game/'lib.so',ns=(1700000000000000000,1700000000000000000),follow_symlinks=False)
assert (game/'v2').stat().st_mtime_ns!=1700000000000000000
os.unlink(game/'lib.so');assert (game/'v2').read_bytes()==b'two'
shutil.rmtree(installed);shutil.rmtree(game)
assert outside.read_bytes()==b'untouched-save' and not installed.exists()
print('PASS depot install/verify/commit/update/remove, directory and file links, dirfd, statx, loops, races, boundaries and user-save preservation')
"""
class DepotLinks(unittest.TestCase):
 def test_depot_lifecycle(self):
  with tempfile.TemporaryDirectory(prefix='steamohos-depot-',dir='/var/tmp') as directory:
   base=Path(directory);(base/'ipc').mkdir(mode=0o700)
   profile=base/'ipc/depot-profile.enabled';profile.touch(mode=0o600)
   compat=base/'compat.so';deny=base/'deny.so';fixture=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/filesystem_compat.c'
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',str(fixture),'-ldl','-o',str(compat)],check=True)
   source=base/'deny.c';source.write_text(r'''
#define _GNU_SOURCE 1
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>
static unsigned long metadata;
unsigned long depot_metadata_count(void){return metadata;}
void depot_metadata_reset(void){metadata=0;}
int lstat(const char *path,struct stat *st){
 static int(*next)(const char*,struct stat*);if(!next)next=dlsym(RTLD_NEXT,"lstat");
 __atomic_fetch_add(&metadata,1,__ATOMIC_RELAXED);return next(path,st);
}
int stat(const char *path,struct stat *st){
 static int(*next)(const char*,struct stat*);if(!next)next=dlsym(RTLD_NEXT,"stat");
 __atomic_fetch_add(&metadata,1,__ATOMIC_RELAXED);return next(path,st);
}
int fstatat(int fd,const char *path,struct stat *st,int flags){
 static int(*next)(int,const char*,struct stat*,int);if(!next)next=dlsym(RTLD_NEXT,"fstatat");
 __atomic_fetch_add(&metadata,1,__ATOMIC_RELAXED);return next(fd,path,st,flags);
}
int symlink(const char*a,const char*b){(void)a;(void)b;errno=EPERM;return -1;}
int symlinkat(const char*a,int d,const char*b){(void)a;(void)b;(void)d;errno=EPERM;return -1;}
''')
   subprocess.run(['cc','-shared','-fPIC',str(source),'-ldl','-o',str(deny)],check=True)
   env={**os.environ,'TEST_BASE':str(base),'STEAMOHOS_LIBRARY_ROOT':str(base/'library'),
    'STEAMOHOS_IPC_ROOT':str(base/'ipc'),'TEST_DENY':str(deny),'LD_PRELOAD':str(compat)+':'+str(deny)}
   result=subprocess.run([sys.executable,'-c',SCRIPT],env=env,capture_output=True,text=True,timeout=30)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('STEAMOHOS_DEPOT_PROFILE',result.stderr)
   print(result.stdout,end='')
   # On normal Linux, preserve genuine kernel symlinks instead of creating records.
   env['LD_PRELOAD']=str(compat)
   result=subprocess.run([sys.executable,'-c',"import os;from pathlib import Path;p=Path(os.environ['TEST_BASE'])/'library/steamapps/common/kernel';p.mkdir();os.symlink('missing',p/'real');assert os.readlink(p/'real')=='missing';(p/'dir/inner').mkdir(parents=True);(p/'dir/sibling').write_text('correct');os.symlink('dir/inner',p/'alias');assert (p/'alias/../sibling').read_text()=='correct'"],env=env,capture_output=True,text=True,timeout=10)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertTrue((base/'library/steamapps/common/kernel/real').is_symlink())
if __name__=='__main__':unittest.main()
