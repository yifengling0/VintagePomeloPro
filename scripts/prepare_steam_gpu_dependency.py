"""Stage the official x86 SDL2 dependency for legacy Steam GPU probes."""
import hashlib, io, struct, urllib.request, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
VERSION='2.32.10'
URL=f'https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL2-{VERSION}-win32-x86.zip'
ZIP_SHA256='d72b0728a315ab25ad43123b89baa2ab6fd7c335a43f737213685d29fd2aa4fe'
DLL_SHA256='68c78590d1997122c30c992ec79857d32e5e976a0f17c0a69a2e29eab0101d70'
OUTPUT=ROOT/'entry/src/main/resources/rawfile/sdl2-x86.dll'
def prepare():
    if OUTPUT.is_file() and hashlib.sha256(OUTPUT.read_bytes()).hexdigest()==DLL_SHA256:
        print('Steam GPU dependency verified: SDL2 '+VERSION+' x86');return
    data=urllib.request.urlopen(URL,timeout=60).read()
    if hashlib.sha256(data).hexdigest()!=ZIP_SHA256:raise RuntimeError('Official SDL2 ZIP checksum mismatch')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        member=next(n for n in archive.namelist() if n.lower().endswith('sdl2.dll'))
        dll=archive.read(member)
    if hashlib.sha256(dll).hexdigest()!=DLL_SHA256:raise RuntimeError('SDL2 DLL checksum mismatch')
    pe=struct.unpack_from('<I',dll,0x3c)[0]
    if dll[:2]!=b'MZ' or dll[pe:pe+4]!=b'PE\x00\x00' or struct.unpack_from('<H',dll,pe+4)[0]!=0x14c:
        raise RuntimeError('Expected official i386 SDL2 PE')
    OUTPUT.parent.mkdir(parents=True,exist_ok=True)
    temporary=OUTPUT.with_suffix('.dll.tmp');temporary.write_bytes(dll);temporary.replace(OUTPUT)
    print('Steam GPU dependency staged: SDL2 '+VERSION+' x86')
if __name__=='__main__':prepare()
