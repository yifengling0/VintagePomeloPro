"""Exercise the production streaming ZIP reader, including real CRC and ZIP64.
Run on Linux: python3 host_tests/steam_zip_test.py (requires C++17 and zlib).
"""
import io
import random
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = r'''
#include "common/steam_zip_reader.h"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
int main(int n,char** a) {
  if(n != 3) return 2;
  int fd=open(a[1],O_RDONLY); uint64_t last=0;
  try { steamzip::Extract(fd,a[2],[&](uint64_t done,uint64_t total) {
    if(done<last || done>total) throw std::runtime_error("bad progress"); last=done;
  }); close(fd); return 0; }
  catch(const std::exception& e) { close(fd); std::cerr<<e.what()<<'\n'; return 1; }
}
'''

def archive(path, prefix='', extras=None, local64=False):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ['steam.exe', 'steamclient.dll', 'steamclient64.dll', 'SteamUI.dll', 'steam.cfg']:
            z.writestr(prefix + name, b'client payload ' * 200)
        if local64:
            with z.open(prefix+'large.bin', 'w', force_zip64=True) as out:
                for _ in range(64):
                    out.write(b'\0' * (1024*1024))
        for name, content in extras or []:
            z.writestr(name, content)

def main():
    with tempfile.TemporaryDirectory(prefix='steam-zip-test-') as tmp:
        root=Path(tmp); (root/'main.cpp').write_text(MAIN)
        binary=root/'reader'
        subprocess.run(['g++','-std=c++17','-O2','-I'+str(ROOT/'entry/src/main/cpp'),
            str(root/'main.cpp'),str(ROOT/'entry/src/main/cpp/common/steam_zip_reader.cpp'),
            '-lz','-o',str(binary)],check=True)
        tests=0
        def run(path, ok=True):
            nonlocal tests
            dest=root/('output-'+str(tests)); tests+=1
            result=subprocess.run([str(binary),str(path),str(dest)],capture_output=True)
            assert (result.returncode==0)==ok, result.stderr.decode(errors='replace')
            if ok: assert (dest/'steam.exe').is_file()
            return dest
        package=root/'root.zip';archive(package);run(package)
        # Incompressible data exhausts a 64 KiB input exactly when output fills.
        archive(package,extras=[('large-random.bin',random.Random(123).randbytes(4*1024*1024))]);run(package)
        archive(package,'Steam/',[('Steam/steamapps/common/中文 游戏/game.exe',b'game')])
        dest=run(package);assert (dest/'steamapps/common/中文 游戏/game.exe').read_bytes()==b'game'
        archive(package,'Steam/',local64=True);dest=run(package)
        assert (dest/'large.bin').stat().st_size==64*1024*1024
        # Upgrade EOCD to ZIP64 without needing 65536 files or a >4GiB fixture.
        data=package.read_bytes();eocd=data[-22:];count=struct.unpack_from('<H',eocd,10)[0]
        size,offset=struct.unpack_from('<II',eocd,12)
        record=struct.pack('<IQHHIIQQQQ',0x06064b50,44,45,45,0,0,count,count,size,offset)
        locator=struct.pack('<IIQI',0x07064b50,0,len(data)-22,1)
        ending=struct.pack('<IHHHHIIH',0x06054b50,0,0,0xffff,0xffff,0xffffffff,0xffffffff,0)
        package.write_bytes(data[:-22]+record+locator+ending);run(package)
        for bad in ['../escape','Steam/../../escape','/absolute','C:/escape','Steam/dir/../escape','Steam/dir\\..\\escape']:
            archive(package,'Steam/',[(bad,b'bad')]);dest=run(package,False)
            assert not dest.exists(), 'unsafe member must fail before extraction'
            assert not (root/'escape').exists()
        archive(package,'Steam/',[('Steam/STEAM.EXE',b'duplicate')]);run(package,False)
        link=zipfile.ZipInfo('Steam/link');link.create_system=3;link.external_attr=0o120777<<16
        archive(package,'Steam/',[(link,b'../../outside')]);run(package,False)
        archive(package)
        data=bytearray(package.read_bytes());central=data.index(b'PK\x01\x02');data[central+16]^=1
        package.write_bytes(data);run(package,False)
        archive(package)
        data=package.read_bytes();package.write_bytes(data[:-15]);run(package,False)
        archive(package,'Wrapper/');run(package,False)
        archive(package,extras=[('bin/vgui2_s.dll',b'legacy')]);run(package,False)
        archive(package,extras=[('bin/vgui2_s.dll',b'legacy'),('steam/games/PlatformMenu.vdf',b'client modules')]);run(package)
        print(f'Steam ZIP: {tests} production reader scenarios passed')

if __name__=='__main__':main()
