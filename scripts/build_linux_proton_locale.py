#!/usr/bin/env python3
"""Build the private prefix locale helper/Windows GDI regression fixture."""
from pathlib import Path
import argparse, hashlib, json, subprocess

def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--windows-cc',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    cc=a.windows_cc.absolute(); assert cc.is_file()
    output=a.output.resolve()
    if output.is_relative_to(root):p.error('Use the existing task directory outside the repository')
    machine=subprocess.check_output([str(cc),'-dumpmachine'],text=True).strip()
    if not machine.startswith('i686-w64-'):p.error('Expected an i686-w64 Windows compiler')
    output.mkdir(parents=True,exist_ok=True)
    source=root/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_locale.c'
    inputs={str(source):sha(source),str(cc):sha(cc)}
    binary=output/'proton-x86-locale.exe'
    record=output/'locale-fixture-identity.json'
    previous=json.loads(record.read_text()) if record.exists() else {}
    if previous.get('inputs')!=inputs or not binary.is_file() or previous.get('sha256')!=sha(binary):
        subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-Wl,--no-insert-timestamp',
            str(source),'-o',str(binary),'-luser32','-lgdi32','-ladvapi32'],check=True)
    record.write_text(json.dumps({'inputs':inputs,'bytes':binary.stat().st_size,'sha256':sha(binary),
        'scope':'GNU private prefix setup; preserves installed fonts and user aliases'},indent=2)+'\n')
    print('Verified locale helper',binary.stat().st_size,sha(binary))

if __name__=='__main__':main()
