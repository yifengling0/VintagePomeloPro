"""Check strip identity against the staged real ARM64 runtime, including stale rejection."""
from pathlib import Path
import json, shutil, sys, tempfile
R=Path(__file__).resolve().parents[1];sys.path.insert(0,str(R/'scripts'))
from normalize_runtime_identity import normalize, load_identity, sha
strip=Path('/apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-strip')
raw=R/'entry/src/main/resources/rawfile'
with tempfile.TemporaryDirectory(prefix='runtime-normalize-test-') as d:
 d=Path(d);libs=d/'libs';out=d/'raw';out.mkdir()
 manifest=json.loads((raw/'wine-runtime-manifest.json').read_text())
 records=[row for key,row in manifest.items() if key in ('sharedAudio','threadExit')]
 records += [dict(row,library='libs/arm64-v8a/'+name) for name,row in manifest.get('graphicsMigration',{}).get('virgl',{}).items()]
 originals={}
 for row in records:
  p=Path(row['library']);target=d/p;target.parent.mkdir(parents=True,exist_ok=True)
  shutil.copy2(R/'entry'/p,target);originals[target]=target.read_bytes()
 shutil.copy2(raw/'wine-runtime-manifest.json',out/'wine-runtime-manifest.json')
 shutil.copy2(raw/'wine_runtime.version',out/'wine_runtime.version')
 normalize(libs,out,strip)
 for path,data in originals.items():assert load_identity(data)==load_identity(path.read_bytes())
 assert normalize(libs,out,strip) is False,'second normalization must be idempotent'
 paired=json.loads((out/'wine-runtime-manifest.json').read_text())
 for key in ('sharedAudio','threadExit'):
  row=paired[key];assert sha((d/row['library']).read_bytes())==row['sha256']
 # A payload/library edit must not be blessed by rehashing it at package time.
 target=next(iter(originals));target.write_bytes(target.read_bytes()+b'stale-input')
 before=(out/'wine-runtime-manifest.json').read_bytes()
 try:normalize(libs,out,strip)
 except ValueError as e:assert 'stale manifest' in str(e)
 else:raise AssertionError('stale ELF was accepted')
 assert (out/'wine-runtime-manifest.json').read_bytes()==before
print('real ARM64 strip identity, paired hashes, idempotence and stale-input rejection PASS')
