"""Exercise native Steam Cloud paths and no-follow compatdata cleanup on OHOS."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CloudPaths(unittest.TestCase):
    def test_native_cloud_roots_and_cleanup(self):
        self.check_cloud_paths(False)

    def test_single_library_cloud_roots_and_cleanup(self):
        self.check_cloud_paths(True)

    def check_cloud_paths(self, single_library):
        with tempfile.TemporaryDirectory(prefix="steamohos-cloud-", dir="/var/tmp") as temporary:
            base = Path(temporary)
            ipc, library, client, prefix = [base / x for x in ("ipc", "library", "client", "wine")]
            profile = prefix / "drive_c/users/steamuser"
            for p in [ipc, library / "steamapps", client / "steamapps", profile / "Documents/My Games/NewPAL",
                      profile / "AppData/Local", profile / "AppData/Roaming", profile / "AppData/LocalLow",
                      profile / "Saved Games", prefix / "drive_c/ProgramData"]:
                p.mkdir(parents=True, exist_ok=True)
            (library / "steamapps/appmanifest_695630.acf").write_text('"AppState" {}')
            (client / "steamapps/appmanifest_42.acf").write_text('"AppState" {}')
            if single_library:
                (library / "steamapps/.hmosdeck-single-library-v2").write_text(str(library))
                (client / "steamapps/appmanifest_42.acf").rename(library / "steamapps/appmanifest_42.acf")
            original = profile / "Documents/My Games/NewPAL/0.sav"
            original.write_bytes(b"original-user-save")
            compat = base / "compat.so"
            fixture = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                str(fixture / "filesystem_compat.c"), "-ldl", "-o", str(compat)], check=True)
            script = r"""
import ctypes, os, shutil, stat
from pathlib import Path
api=ctypes.CDLL(None)
api.realpath.argtypes=[ctypes.c_char_p,ctypes.c_void_p]
api.realpath.restype=ctypes.c_void_p
def native_realpath(path):
 buffer=ctypes.create_string_buffer(4096)
 assert api.realpath(os.fsencode(path),buffer)
 return Path(os.fsdecode(buffer.value))
assert api.SteamOhosPrepareCloudPrefixes()==2
library=Path(os.environ['STEAMOHOS_LIBRARY_ROOT'])
client=Path(os.environ['STEAMOHOS_STEAM_ROOT'])
prefix=Path(os.environ['STEAMOHOS_WINE_PREFIX'])
ipc=Path(os.environ['STEAMOHOS_IPC_ROOT'])
pfx=library/'steamapps/compatdata/695630/pfx'
user=pfx/'drive_c/users/steamuser'
assert stat.S_ISLNK(os.lstat(user).st_mode)
assert os.readlink(user)==str(prefix/'drive_c/users/steamuser')
assert user.resolve()==prefix/'drive_c/users/steamuser'
assert (user/'My Documents/My Games/NewPAL/0.sav').read_bytes()==b'original-user-save'
assert native_realpath(user/'Local Settings/Application Data')==prefix/'drive_c/users/steamuser/AppData/Local'
assert native_realpath(user/'Application Data')==prefix/'drive_c/users/steamuser/AppData/Roaming'
assert native_realpath(pfx/'drive_c/ProgramData')==prefix/'drive_c/ProgramData'
assert (client/'steamapps/compatdata/42/pfx/drive_c/users/steamuser').is_dir()
# Real Cloud-style atomic download into the real game directory, then removal.
output=user/'My Documents/My Games/NewPAL/cloud-test.sav'
output.with_suffix('.tmp').write_bytes(b'cloud-test')
os.rename(output.with_suffix('.tmp'),output)
assert (prefix/'drive_c/users/steamuser/Documents/My Games/NewPAL/cloud-test.sav').read_bytes()==b'cloud-test'
# Steam preserves downloaded remote mtimes with utime/utimes.
class Utime(ctypes.Structure):
 _fields_=[('access',ctypes.c_long),('modified',ctypes.c_long)]
api.utime.argtypes=[ctypes.c_char_p,ctypes.POINTER(Utime)]
assert api.utime(os.fsencode(output),ctypes.byref(Utime(1700000000,1700000123)))==0
assert int(output.stat().st_mtime)==1700000123
class Timeval(ctypes.Structure):
 _fields_=[('seconds',ctypes.c_long),('micros',ctypes.c_long)]
times=(Timeval*2)(Timeval(1700000000,0),Timeval(1700000456,123000))
api.utimes.argtypes=[ctypes.c_char_p,ctypes.POINTER(Timeval)]
assert api.utimes(os.fsencode(output),times)==0
assert abs(output.stat().st_mtime-1700000456.123)<0.00001
output.unlink()
# Relative *at APIs retain normal directory/symlink semantics.
fd=os.open(user.parent,os.O_RDONLY|os.O_DIRECTORY)
assert stat.S_ISLNK(os.stat('steamuser',dir_fd=fd,follow_symlinks=False).st_mode)
assert os.readlink('steamuser',dir_fd=fd)==str(prefix/'drive_c/users/steamuser')
# Traverse from a metadata parent fd through the profile link, then perform
# actual atomic save IO using *at APIs.
leaf='steamuser/My Documents/My Games/NewPAL/fd-test.sav'
file=os.open(leaf,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600,dir_fd=fd)
os.write(file,b'fd-save');os.close(file)
os.rename(leaf,leaf+'.renamed',src_dir_fd=fd,dst_dir_fd=fd)
os.unlink(leaf+'.renamed',dir_fd=fd)
previous=os.getcwd();os.chdir(user.parent)
assert stat.S_ISLNK(os.lstat('steamuser').st_mode)
assert os.readlink('steamuser')==str(prefix/'drive_c/users/steamuser')
try:os.open('steamuser',os.O_RDONLY|os.O_NOFOLLOW)
except OSError as error:assert error.errno==40
else:raise AssertionError('Relative alias was followed')
os.chdir(previous);os.close(fd)
# Invalid IDs, sibling libraries and path escapes must not be redirected.
for invalid in ['0','-1','4294967296','695630x']:
 assert not (library/('steamapps/compatdata/'+invalid+'/pfx')).exists()
assert not (library/'steamapps/compatdata-other/695630/pfx').exists()
other=library.parent/'WindowsSteam/steamapps/compatdata/695630/pfx'
assert not other.exists()
assert not (library/'steamapps/compatdata/695630/../../escape').exists()
# O_NOFOLLOW / O_EXCL must observe the link itself.
import errno
for flags,wanted in [(os.O_RDONLY|os.O_NOFOLLOW,errno.ELOOP),(os.O_CREAT|os.O_EXCL|os.O_WRONLY,errno.EEXIST)]:
 try:os.open(user,flags,0o600)
 except OSError as error:assert error.errno==wanted
 else:raise AssertionError('Link incorrectly followed')
# Fortified glibc calls used by native clients also preserve no-follow.
api.__open_2.argtypes=[ctypes.c_char_p,ctypes.c_int]
assert api.__open_2(os.fsencode(user),os.O_RDONLY|os.O_NOFOLLOW)==-1
# No kernel symlinks or aliases were created in the real Wine profile.
assert not os.path.lexists(prefix/'drive_c/users/steamuser/My Documents')
# Repeated setup is idempotent; rm -r removes links, never user saves.
assert api.SteamOhosPrepareCloudPrefixes()==2
shutil.rmtree(library/'steamapps/compatdata/695630')
assert not (ipc/'hmosdeck-cloud-compatdata/695630').exists()
assert (prefix/'drive_c/users/steamuser/Documents/My Games/NewPAL/0.sav').read_bytes()==b'original-user-save'
assert not (library/'steamapps/compatdata/695630').exists(), 'No lazy recreation during cleanup'
print('PASS native Proton Cloud roots, atomic IO, dirfd, scope boundaries and save-preserving cleanup')
"""
            environment = {**os.environ, "LD_PRELOAD": str(compat), "STEAMOHOS_IPC_ROOT": str(ipc),
                "STEAMOHOS_LIBRARY_ROOT": str(library), "STEAMOHOS_STEAM_ROOT": str(client),
                "STEAMOHOS_WINE_PREFIX": str(prefix)}
            result = subprocess.run([sys.executable, "-c", script], env=environment,
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(original.read_bytes(), b"original-user-save")
            print(result.stdout, end="")

if __name__ == "__main__":
    unittest.main()
