#!/usr/bin/env python3
"""Generate only Wine's four supported UTF-8 locales; no rootfs or archive."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--localedef', default='/usr/bin/localedef')
a=p.parse_args(); a.output.mkdir(parents=True,exist_ok=True)
languages=['zh_CN','zh_TW','ja_JP','en_US']
inputs={str(q):hashlib.sha256(q.read_bytes()).hexdigest() for q in
        [Path(a.localedef),Path('/usr/share/i18n/charmaps/UTF-8.gz'),
         *sorted(Path('/usr/share/i18n/locales').iterdir())] if q.is_file()}
record=a.output/'locale-identity.json'
old=json.loads(record.read_text()) if record.exists() else {}
def identity():
    return {str(q.relative_to(a.output)):{'bytes':q.stat().st_size,
            'sha256':hashlib.sha256(q.read_bytes()).hexdigest()}
            for name in languages for q in sorted((a.output/(name+'.UTF-8')).rglob('*')) if q.is_file()}
files=identity()
if old.get('inputs')==inputs and old.get('files')==files and files:
    print('Reused verified GNU locale data',sum(x['bytes'] for x in files.values()))
else:
    for name in languages:
        subprocess.run([a.localedef,'--no-archive','--little-endian','-i',name,'-f','UTF-8',
                        str(a.output/(name+'.UTF-8'))],check=True)
    files=identity()
    record.write_text(json.dumps({'inputs':inputs,'files':files,
        'abi':'glibc little-endian locale data; validate setlocale with target libc',
        'bytes':sum(x['bytes'] for x in files.values())},indent=2)+'\n')
    print('Generated GNU locales',sum(x['bytes'] for x in files.values()))
