#!/usr/bin/env python3
"""Compute a hashed PE dependency closure for a private GNU Proton prefix."""
from pathlib import Path
import argparse,hashlib,json,struct,tarfile

def imports(data):
    if data[:2]!=b'MZ': raise ValueError('Not a PE image')
    header=struct.unpack_from('<I',data,60)[0]
    if data[header:header+4]!=b'PE\0\0': raise ValueError('Invalid PE signature')
    count,optional=struct.unpack_from('<H12xH',data,header+6)
    start=header+24;magic=struct.unpack_from('<H',data,start)[0]
    directories=start+(112 if magic==0x20b else 96 if magic==0x10b else 0)
    if directories==start: raise ValueError('Unsupported PE optional header')
    sections=[struct.unpack_from('<IIII',data,start+optional+n*40+8) for n in range(count)]
    def offset(rva):
        for virtual_size,virtual,raw_size,raw in sections:
            if virtual<=rva<virtual+max(virtual_size,raw_size): return raw+rva-virtual
        if rva<struct.unpack_from('<I',data,start+60)[0]: return rva
        raise ValueError('Unmapped PE RVA')
    def string(rva):
        pos=offset(rva);return data[pos:data.index(b'\0',pos)].decode('ascii').lower()
    names=set()
    rva,size=struct.unpack_from('<II',data,directories+8)
    if rva:
        table=offset(rva)
        for position in range(table,min(table+size,len(data)-19),20):
            row=struct.unpack_from('<IIIII',data,position)
            if not any(row): break
            names.add(string(row[3]))
    # Forwarded exports are part of the closure too (e.g. kernel32 -> kernelbase).
    rva,size=struct.unpack_from('<II',data,directories)
    if rva:
        position=offset(rva);functions=struct.unpack_from('<I',data,position+20)[0]
        table=offset(struct.unpack_from('<I',data,position+28)[0]) if functions else 0
        for n in range(functions):
            address=struct.unpack_from('<I',data,table+n*4)[0]
            if rva<=address<rva+size:
                name=string(address).split('.')[0]
                if name:names.add(name+'.dll')
    return names

def hydrate(args):
    lock=json.loads(args.lock.read_text())
    archive=args.archive.resolve(strict=True);dist=lock['distribution']
    digest=hashlib.sha512()
    with archive.open('rb') as stream:
        for chunk in iter(lambda:stream.read(4*1024*1024),b''):digest.update(chunk)
    if archive.stat().st_size!=dist['bytes'] or digest.hexdigest()!=dist['sha512']:
        raise ValueError('Proton archive identity mismatch')
    index_path=args.output.parent/'pe-archive-index.json'
    cached=json.loads(index_path.read_text()) if index_path.is_file() else {}
    tables=cached.get('architectures',{}) if cached.get('archiveSha512')==dist['sha512'] else {}
    arch=args.arch+'-windows'
    if arch not in tables:
        modules={}
        with tarfile.open(archive,'r|gz') as stream:
            for member in stream:
                parts=Path(member.name).parts
                if len(parts)!=6 or parts[1:4]!=('files','lib','wine') or parts[4]!=arch or not member.isfile():continue
                if member.size>64*1024*1024:raise ValueError('Oversized PE member')
                data=stream.extractfile(member).read()
                if data[:2]!=b'MZ':continue
                try:dependencies=imports(data)
                except (ValueError,struct.error):continue # 16-bit support files are not PE modules.
                modules[parts[-1].lower()]={'path':'/'.join(parts[1:]),'bytes':len(data),'type':'0','link':'',
                    'sha256':hashlib.sha256(data).hexdigest(),'imports':sorted(dependencies)}
        tables[arch]=modules
        index_path.write_text(json.dumps({'archiveSha512':dist['sha512'],'architectures':tables},indent=2)+'\n')
    modules=tables[arch];pending=({name for name in modules if name.endswith(('.dll','.drv'))} if args.all_libraries else set(n.lower() for n in args.seed));visited=set();selected={}
    while pending:
        name=pending.pop()
        if name in visited:continue
        visited.add(name)
        row=modules.get(name)
        if row: selected[row['path']]=row;pending.update(set(row['imports'])-visited)
    missing={}
    for name,row in selected.items():
        target=args.package/name
        if not target.resolve().is_relative_to(args.package.resolve()):raise ValueError('Unsafe PE target')
        if not target.is_file() or target.stat().st_size!=row['bytes'] or hashlib.sha256(target.read_bytes()).hexdigest()!=row['sha256']:
            if target.exists():raise ValueError('Existing PE content differs from locked archive: '+name)
            missing[name]=row
    if missing:
        with tarfile.open(archive,'r|gz') as stream:
            for member in stream:
                name='/'.join(Path(member.name).parts[1:]);row=missing.get(name)
                if not row:continue
                if not member.isfile() or member.size!=row['bytes']:raise ValueError('PE member changed')
                data=stream.extractfile(member).read()
                if hashlib.sha256(data).hexdigest()!=row['sha256']:raise ValueError('PE checksum mismatch')
                target=args.package/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
                del missing[name]
        if missing:raise ValueError('Missing archive members')
    known={r['path'] for r in lock['selected']};added=[]
    for name,row in selected.items():
        if name not in known:
            record={k:v for k,v in row.items() if k!='imports'};lock['selected'].append(record);added.append(record)
    if added:
        lock['selectedBytes']=sum(r['bytes'] for r in lock['selected'] if r['type']=='0')
        args.lock.write_text(json.dumps(lock,indent=2)+'\n')
        (args.output.parent/'package-bootstrap-inventory.json').write_text(json.dumps(lock,indent=2)+'\n')
    (args.output.with_suffix('.additional.json')).write_text(json.dumps(added,indent=2)+'\n')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--arch',choices=['i386','aarch64','x86_64'],default='i386')
    parser.add_argument('--seed',action='append',default=[])
    parser.add_argument('--all-libraries',action='store_true',help='Install the distribution library set, including dynamically loaded DLLs')
    parser.add_argument('--archive',type=Path,help='Resolve missing transitive modules from the existing locked archive')
    parser.add_argument('--lock',type=Path,default=Path(__file__).resolve().parents[1]/'docs/linux-steam/gnu-proton-probe.lock.json')
    args=parser.parse_args()
    if not args.seed and not args.all_libraries:parser.error('Select --seed or --all-libraries')
    if args.archive:hydrate(args)
    root=args.package.resolve(strict=True)/('files/lib/wine/'+args.arch+'-windows')
    files={p.name.lower():p for p in root.iterdir() if p.is_file() and not p.is_symlink()}
    pending=({name for name in files if name.endswith(('.dll','.drv'))} if args.all_libraries else set(x.lower() for x in args.seed));visited=set();rows=[];external=[]
    while pending:
        name=pending.pop()
        if name in visited:continue
        visited.add(name)
        if name not in files:external.append(name);continue
        path=files[name];data=path.read_bytes();pending.update(imports(data)-visited)
        rows.append({'path':str(path.relative_to(args.package.resolve())), 'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
    rows.sort(key=lambda x:x['path'])
    args.output.write_text(json.dumps({'architecture':args.arch,'seeds':args.seed,'modules':rows,'externalOrApiSets':sorted(external),'selectedBytes':sum(r['bytes'] for r in rows)},indent=2)+'\n')
    print('Selected',len(rows),'PE modules,',sum(r['bytes'] for r in rows),'bytes')
if __name__=='__main__':main()
