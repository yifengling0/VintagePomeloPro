"""Add a hash-guarded exception to the exact legacy SteamUI client loader.

Three call sites in InternalAPI_Init_Internal are redirected; the shared
signature verifier is unchanged. Unknown files still use the original check.
"""
from pathlib import Path
import hashlib
import json
import struct
import pefile

def align(value, alignment):
    return (value+alignment-1)//alignment*alignment

SOURCE_SHA256 = 'f08296fb1345e489f97f7d36a2740d899e8a3a3aca0537e300bc9fda0e5e45f1'
CALLS = (0x1a3d74, 0x1a3dce, 0x1a3e63)
ORIGINAL_VERIFIER = 0x74ed80

def patch_startup(data):
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('Unsupported or already patched SteamUI.dll')
    pe = pefile.PE(data=data)
    if pe.FILE_HEADER.Machine != 0x14c:
        raise ValueError('Unexpected SteamUI machine')
    for site in CALLS:
        if pe.get_data(site, 5) != b'\xe8' + struct.pack('<i', ORIGINAL_VERIFIER-site-5):
            raise ValueError('Unexpected client-loader signature call')
    header_at = pe.sections[-1].get_file_offset()+40
    if header_at+40 > min(s.PointerToRawData for s in pe.sections if s.SizeOfRawData) or any(data[header_at:header_at+40]):
        raise ValueError('No empty section-header space')
    rva = align(max(pe.OPTIONAL_HEADER.SizeOfImage,
        max(s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData) for s in pe.sections)), pe.OPTIONAL_HEADER.SectionAlignment)
    raw_at = align(len(data), pe.OPTIONAL_HEADER.FileAlignment)
    old_imports = pe.get_data(pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress, len(pe.DIRECTORY_ENTRY_IMPORT)*20)
    payload = bytearray(old_imports+b'\0'*40)
    descriptor_size = len(payload)
    def add(blob, alignment=1):
        at = align(len(payload), alignment)
        payload.extend(b'\0'*(at-len(payload)))
        payload.extend(blob)
        return at
    name = add(b'vpsteamtrust32.dll\0')
    hint = add(b'\0\0VPValidateLegacyClient\0', 2)
    ilt = add(struct.pack('<II', rva+hint, 0), 4)
    iat = add(struct.pack('<II', rva+hint, 0), 4)
    struct.pack_into('<IIIII', payload, len(old_imports), rva+ilt, 0, 0, rva+name, rva+iat)
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    thunk = text.VirtualAddress+text.Misc_VirtualSize
    # Original cdecl stack: return address, filename. Pass filename and the
    # original verifier to the helper, then return its result without altering
    # the caller's argument or any callee-saved registers.
    code = b'\x68' + struct.pack('<I', pe.OPTIONAL_HEADER.ImageBase+ORIGINAL_VERIFIER)
    code += b'\xff\x74\x24\x08\xff\x15' + struct.pack('<I', pe.OPTIONAL_HEADER.ImageBase+rva+iat)
    code += b'\x83\xc4\x08\xc3'
    if len(code) > text.SizeOfRawData-text.Misc_VirtualSize or any(pe.get_data(thunk, len(code))):
        raise ValueError('No verified executable padding')
    code_at = pe.get_offset_from_rva(thunk)
    old_reloc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]
    reloc = add(pe.get_data(old_reloc.VirtualAddress, old_reloc.Size), 4)
    blocks = {}
    for operand in (thunk+1, thunk+11):
        blocks.setdefault(operand&~0xfff, []).append(0x3000 | (operand&0xfff))
    for page, entries in sorted(blocks.items()):
        if len(entries)%2:
            entries.append(0)
        add(struct.pack('<II', page, 8+len(entries)*2)+struct.pack('<'+'H'*len(entries), *entries), 4)
    old_reloc.VirtualAddress = rva+reloc
    old_reloc.Size = len(payload)-reloc
    text.Misc_VirtualSize += len(code)
    raw_size = align(len(payload), pe.OPTIONAL_HEADER.FileAlignment)
    pe.FILE_HEADER.NumberOfSections += 1
    pe.OPTIONAL_HEADER.SizeOfImage = align(rva+len(payload), pe.OPTIONAL_HEADER.SectionAlignment)
    pe.OPTIONAL_HEADER.SizeOfInitializedData += raw_size
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress = rva
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].Size = descriptor_size
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].VirtualAddress = 0
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[4].Size = 0
    result = bytearray(pe.write())
    result[header_at:header_at+40] = struct.pack('<8sIIIIIIHHI', b'.vptr\0\0\0', len(payload), rva, raw_size, raw_at, 0,0,0,0,0xc0000040)
    result[code_at:code_at+len(code)] = code
    for site in CALLS:
        at = pe.get_offset_from_rva(site)
        result[at:at+5] = b'\xe8'+struct.pack('<i', thunk-site-5)
    result.extend(b'\0'*(raw_at-len(result)))
    result += payload+b'\0'*(raw_size-len(payload))
    final = pefile.PE(data=bytes(result))
    final.OPTIONAL_HEADER.CheckSum = final.generate_checksum()
    output = final.write()
    verified = pefile.PE(data=output)
    assert [d.dll for d in verified.DIRECTORY_ENTRY_IMPORT] == [d.dll for d in pe.DIRECTORY_ENTRY_IMPORT]+[b'vpsteamtrust32.dll']
    assert verified.get_data(ORIGINAL_VERIFIER, 0x97) == pe.get_data(ORIGINAL_VERIFIER, 0x97)
    return output, dict(source_sha256=SOURCE_SHA256, patched_sha256=hashlib.sha256(output).hexdigest(),
                        client_load_calls=[hex(x) for x in CALLS], thunk_rva=hex(thunk),
                        unchanged_shared_verifier=hex(ORIGINAL_VERIFIER), hash_guard='exact client and decoder SHA256')

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('input', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    if args.output.exists():
        raise ValueError('Output must be new')
    output, manifest = patch_startup(args.input.read_bytes())
    args.output.write_bytes(output)
    args.output.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))
