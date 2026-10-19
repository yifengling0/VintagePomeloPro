"""Verify final-module identity checks reject stale MF, VirGL and lock records.
Minimal archives exercise the identity layer; the complete HAP has a separate
runtime closure gate. All module bytes come from the actual candidate.
"""
import argparse, hashlib, io, json, subprocess, tempfile
from pathlib import Path
from zipfile import ZipFile
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--hap',type=Path,required=True);a=p.parse_args()
def pack(members):
 b=io.BytesIO()
 with ZipFile(b,'w') as z:
  for n,data in members.items():z.writestr(n,data)
 return b.getvalue()
def sha(b):return hashlib.sha256(b).hexdigest()
M='resources/rawfile/wine-runtime-manifest.json';P='resources/rawfile/wine-data.zip'
with ZipFile(a.hap) as hap:
 manifest=json.loads(hap.read(M));record=manifest['graphicsMigration']
 with ZipFile(io.BytesIO(hap.read(P))) as wine:
  dlls={n:wine.read(n) for n in record['mfplat']}
 members={'module.json':hap.read('module.json')}
 members.update({'libs/arm64-v8a/'+n:hap.read('libs/arm64-v8a/'+n) for n in record['virgl']})
with tempfile.TemporaryDirectory(prefix='graphics-package-identity-') as d:
 for kind,expected in [('valid',None),('lock','packaged migration lock'),('virgl','packaged VirGL'),('mf','packaged MF')]:
  current=dict(members);ds=dict(dlls);m=json.loads(json.dumps(manifest))
  if kind=='lock':m['graphicsMigration']['lockSha256']='0'*64
  if kind=='virgl':current['libs/arm64-v8a/libvirglrenderer.so.1']+=b'stale'
  if kind=='mf':ds['bin/aarch64-windows/mfplat.dll']+=b'stale'
  current[P]=pack(ds);m['payloadSha256']=sha(current[P]);current[M]=json.dumps(m).encode()
  archive=Path(d)/(kind+'.hap');archive.write_bytes(pack(current))
  result=subprocess.run(['python3',str(R/'scripts/verify_graphics_contract.py'),'--hap',str(archive)],text=True,capture_output=True)
  if expected:assert result.returncode!=0 and expected in result.stdout+result.stderr,result.stdout+result.stderr
  else:assert result.returncode==0,result.stdout+result.stderr
print('final package identity: valid modules pass; stale MF, VirGL and lock rejected PASS')
