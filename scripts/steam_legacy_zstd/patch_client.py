"""Add a guarded, format-specific import thunk to two exact legacy Steam DLLs.

Only VSZa goes to our helper. Other formats replay the stolen prologue and
continue through the unmodified decoder. The input SHA256 must match; unknown
or previously patched versions are rejected. Requires pefile on the build host.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import struct
import zipfile
import pefile

PROFILES = json.loads((Path(__file__).parent/'profiles.json').read_text())

def align(value, alignment):
    return (value+alignment-1)//alignment*alignment

def patch(data, name, diagnostic=False):
    profile=PROFILES[name]
    if hashlib.sha256(data).hexdigest()!=profile['sha256']:
        raise ValueError('Unsupported or already patched '+name)
    pe=pefile.PE(data=data)
    if pe.FILE_HEADER.Machine!=profile['machine']:
        raise ValueError('Unexpected PE machine')
    stolen=bytes.fromhex(profile['stolen_bytes'])
    entry=profile['decoder_rva']
    if pe.get_data(entry,len(stolen))!=stolen:
        raise ValueError('Unexpected decoder prologue')
    section_header=pe.sections[-1].get_file_offset()+40
    if section_header+40>min(s.PointerToRawData for s in pe.sections if s.SizeOfRawData):
        raise ValueError('No room for section header')
    if any(data[section_header:section_header+40]):
        raise ValueError('Section header padding is occupied')
    ptr_size=8 if profile['machine']==0x8664 else 4
    file_align=pe.OPTIONAL_HEADER.FileAlignment
    sec_align=pe.OPTIONAL_HEADER.SectionAlignment
    rva=align(max(pe.OPTIONAL_HEADER.SizeOfImage,
        max(s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData) for s in pe.sections)),sec_align)
    raw_offset=align(len(data),file_align)
    old_imports=pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress,
        len(pe.DIRECTORY_ENTRY_IMPORT)*20)
    desc_size=len(old_imports)+40
    payload=bytearray(desc_size)
    payload[:len(old_imports)]=old_imports

    def add(blob,alignment=1):
        offset=align(len(payload),alignment)
        payload.extend(b'\0'*(offset-len(payload)))
        payload.extend(blob)
        return offset

    dll_name=add(profile['helper'].encode()+b'\0')
    hint_name=add(b'\0\0'+(b'VPDecodeDispatch' if diagnostic else b'VPDecodeVSZa')+b'\0',2)
    fmt='<Q' if ptr_size==8 else '<I'
    ilt=add(struct.pack(fmt,rva+hint_name)+b'\0'*ptr_size,ptr_size)
    iat=add(struct.pack(fmt,rva+hint_name)+b'\0'*ptr_size,ptr_size)
    struct.pack_into('<IIIII',payload,len(old_imports),rva+ilt,0,0,rva+dll_name,rva+iat)
    old_reloc=pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]
    reloc_offset=None
    if ptr_size==4:
        reloc_offset=add(pe.get_data(old_reloc.VirtualAddress,old_reloc.Size),4)
        # One HIGHLOW relocation for the x86 absolute IAT operand.
        reloc_extra=add(b'\0'*12,4)
        if reloc_extra!=reloc_offset+old_reloc.Size:
            raise ValueError('Unexpected relocation alignment')
    # The exact binaries have verified zero padding in their existing RX text
    # section. Put the small thunk there and keep the new import section RW.
    # Windows requires the added IAT to be writable during initial fixups.
    text=next(s for s in pe.sections if s.Name.startswith(b'.text'))
    code_rva=text.VirtualAddress+text.Misc_VirtualSize
    code=bytearray()
    jumps=[]
    if diagnostic and ptr_size==4:
        # Obtain the original trampoline address without another absolute
        # relocation, then forward all four arguments and the fifth callback.
        code+=b'\xe8\0\0\0\0\x58\x05'+b'\0'*4
        original_delta_at=7
        code+=b'\x50'+bytes.fromhex('ff742414')*4
        code+=b'\xff\x15'
        operand_rva=code_rva+len(code)
        code+=struct.pack('<I',pe.OPTIONAL_HEADER.ImageBase+rva+iat)
        code+=bytes.fromhex('83c414c3')
        page=operand_rva&~0xfff
        struct.pack_into('<IIHH',payload,reloc_extra,page,12,0x3000|(operand_rva&0xfff),0)
        struct.pack_into('<i',code,original_delta_at,len(code)-5)
    elif diagnostic:
        # Windows x64: preserve the caller's four argument registers, provide
        # shadow space and align RSP before calling the five-argument wrapper.
        code+=bytes.fromhex('4883ec38488d05')+b'\0'*4
        original_delta_at=7
        code+=bytes.fromhex('4889442420')
        code+=b'\xff\x15'+struct.pack('<i',rva+iat-(code_rva+len(code)+6))
        code+=bytes.fromhex('4883c438c3')
        struct.pack_into('<i',code,original_delta_at,len(code)-11)
    elif ptr_size==4:
        code+=bytes.fromhex('8b442404837c240804')
        code+=b'\x0f\x82'+b'\0'*4; jumps.append(len(code)-4)
        code+=bytes.fromhex('813856535a61')
        code+=b'\x0f\x85'+b'\0'*4; jumps.append(len(code)-4)
        code+=b'\xff\x25'
        operand_rva=code_rva+len(code)
        code+=struct.pack('<I',pe.OPTIONAL_HEADER.ImageBase+rva+iat)
        page=operand_rva&~0xfff
        struct.pack_into('<IIHH',payload,reloc_extra,page,12,0x3000|(operand_rva&0xfff),0)
    else:
        code+=bytes.fromhex('83fa04')
        code+=b'\x0f\x82'+b'\0'*4; jumps.append(len(code)-4)
        code+=bytes.fromhex('813956535a61')
        code+=b'\x0f\x85'+b'\0'*4; jumps.append(len(code)-4)
        code+=b'\xff\x25'+struct.pack('<i',rva+iat-(code_rva+len(code)+6))
    fallback=len(code)
    code+=stolen
    code+=b'\xe9'+struct.pack('<i',entry+len(stolen)-(code_rva+len(code)+5))
    for at in jumps:
        struct.pack_into('<i',code,at,fallback-(at+4))
    slack=text.SizeOfRawData-text.Misc_VirtualSize
    if len(code)>slack or any(pe.get_data(code_rva,len(code))):
        raise ValueError('No verified executable padding for thunk')
    code_file_offset=pe.get_offset_from_rva(code_rva)
    text.Misc_VirtualSize+=len(code)
    raw_size=align(len(payload),file_align)
    pe.FILE_HEADER.NumberOfSections+=1
    pe.OPTIONAL_HEADER.SizeOfImage=align(rva+len(payload),sec_align)
    pe.OPTIONAL_HEADER.SizeOfInitializedData+=raw_size
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress=rva
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].Size=desc_size
    # Any binary modification invalidates the original Authenticode signature.
    # Keep its bytes in the overlay but do not advertise it as a valid signature.
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress=0
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size=0
    if reloc_offset is not None:
        old_reloc.VirtualAddress=rva+reloc_offset
        old_reloc.Size+=12
    output=bytearray(pe.write())
    # Writable import data and existing RX code; no RWX section or runtime
    # code patching is needed.
    header=struct.pack('<8sIIIIIIHHI',b'.vpsz\0\0\0',len(payload),rva,raw_size,
        raw_offset,0,0,0,0,0xc0000040)
    output[section_header:section_header+40]=header
    at=pe.get_offset_from_rva(entry)
    output[at:at+len(stolen)]=b'\xe9'+struct.pack('<i',code_rva-(entry+5))+b'\x90'*(len(stolen)-5)
    output[code_file_offset:code_file_offset+len(code)]=code
    output.extend(b'\0'*(raw_offset-len(output)))
    output+=payload+b'\0'*(raw_size-len(payload))
    patched=pefile.PE(data=bytes(output))
    patched.OPTIONAL_HEADER.CheckSum=patched.generate_checksum()
    result=patched.write()
    old_names=[d.dll for d in pe.DIRECTORY_ENTRY_IMPORT]
    new_names=[d.dll for d in patched.DIRECTORY_ENTRY_IMPORT]
    if new_names!=old_names+[profile['helper'].encode()]:
        raise ValueError('Import preservation failed')
    return result, dict(name=name,source_sha256=profile['sha256'],
        patched_sha256=hashlib.sha256(result).hexdigest(),decoder_rva=hex(entry),
        thunk_rva=hex(code_rva),helper=profile['helper'],section='RW data; existing RX code',
        diagnostic=diagnostic,
        original_signature='invalidated by the requested binary modification')

def patch_zip(source,output,helpers,diagnostic=False):
    source=Path(source).resolve(); output=Path(output).resolve()
    if source==output or output.exists():
        raise ValueError('Output must be a new file; original archive is preserved')
    with zipfile.ZipFile(source) as archive:
        names=archive.namelist()
        for member in names:
            normalized=PurePosixPath(member.replace('\\','/'))
            if normalized.is_absolute() or '..' in normalized.parts or ':' in str(normalized):
                raise ValueError('Unsafe archive member path')
        selected={}
        for name in (*PROFILES, 'SteamUI.dll'):
            candidates=[n for n in names if PurePosixPath(n).name.lower()==name.lower()]
            if len(candidates)!=1: raise ValueError('Expected exactly one '+name)
            selected[name]=candidates[0]
        parents={str(PurePosixPath(n).parent) for n in selected.values()}
        if len(parents)!=1: raise ValueError('Client DLLs must share one directory')
        parent=parents.pop()
        modified={}; records=[]
        for name,member in selected.items():
            if name=='SteamUI.dll':
                from patch_startup import patch_startup
                changed,record=patch_startup(archive.read(member))
                record['name']=name
            else:
                changed,record=patch(archive.read(member),name,diagnostic=diagnostic)
            modified[member]=changed; records.append(record)
        added={parent+'/'+p['helper']: (Path(helpers)/p['helper']).read_bytes() for p in PROFILES.values()}
        added[parent+'/vpsteamtrust32.dll']=(Path(helpers)/'vpsteamtrust32.dll').read_bytes()
        added[parent+'/VPP-ZSTD-README.txt']=(Path(__file__).parent/'README.md').read_bytes()
        added[parent+'/VPP-ZSTD-LICENSE.txt']=(Path(__file__).parent/'ZSTD-LICENSE.txt').read_bytes()
        if any(n in names for n in added): raise ValueError('Helper already exists in input')
        private_dirs=('config','userdata','logs','appcache','dumps')
        removed={kind:0 for kind in (*private_dirs,'ssfn')}
        output.parent.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as dest:
            for info in archive.infolist():
                lower=info.filename.lower()
                private_kind=next((kind for kind in private_dirs
                    if lower.startswith(parent.lower()+'/'+kind+'/')),None)
                if PurePosixPath(lower).name.startswith('ssfn'):
                    private_kind='ssfn'
                if private_kind:
                    removed[private_kind]+=1
                    continue
                if info.filename in modified:
                    dest.writestr(info,modified[info.filename])
                else:
                    with archive.open(info) as src, dest.open(info,'w') as dst:
                        while block:=src.read(1024*1024): dst.write(block)
            for name,data in added.items():
                info=zipfile.ZipInfo(name,date_time=(2026,10,7,0,0,0))
                info.compress_type=zipfile.ZIP_DEFLATED
                dest.writestr(info,data)
    manifest=dict(source_sha256=hashlib.file_digest(source.open('rb'),'sha256').hexdigest(),
        output_sha256=hashlib.file_digest(output.open('rb'),'sha256').hexdigest(),
        modified=records,private_user_state_removed=removed,
        added=[dict(path=n,bytes=len(d),sha256=hashlib.sha256(d).hexdigest()) for n,d in added.items()])
    output.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

def make_overlay(full, output):
    output=Path(output).resolve()
    if output.exists():
        raise ValueError('Overlay output must be new')
    expected={'steamclient.dll','steamclient64.dll','vpsteamzstd32.dll','vpsteamzstd64.dll',
              'SteamUI.dll','vpsteamtrust32.dll','VPP-ZSTD-README.txt','VPP-ZSTD-LICENSE.txt'}
    selected={}
    with zipfile.ZipFile(full) as source:
        for info in source.infolist():
            name=PurePosixPath(info.filename).name
            if name in expected:
                if name in selected:
                    raise ValueError('Duplicate overlay member')
                selected[name]=source.read(info.filename)
    if set(selected)!=expected:
        raise ValueError('Incomplete overlay')
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as dest:
        for name,data in sorted(selected.items()):
            info=zipfile.ZipInfo(name,date_time=(2026,10,7,0,0,0))
            info.compress_type=zipfile.ZIP_DEFLATED
            dest.writestr(info,data)
    manifest=dict(name=output.name,bytes=output.stat().st_size,
        sha256=hashlib.file_digest(output.open('rb'),'sha256').hexdigest(),
        members=[dict(name=n,sha256=hashlib.sha256(d).hexdigest()) for n,d in sorted(selected.items())])
    output.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--zip',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--helpers',required=True,type=Path)
    parser.add_argument('--overlay',type=Path)
    parser.add_argument('--diagnostic',action='store_true',help='Use matching diagnostic helpers and exact startup hash guard')
    args=parser.parse_args()
    if args.overlay and (args.overlay.exists() or args.overlay.resolve()==args.output.resolve()):
        raise ValueError('Overlay output must be a separate new file')
    print(json.dumps(patch_zip(args.zip,args.output,args.helpers,diagnostic=args.diagnostic),indent=2))
    if args.overlay:
        print(json.dumps(make_overlay(args.output,args.overlay),indent=2))
