"""Package a sanitized runtime32 Steam bundle with the pinned x86 GPU dependency."""
import argparse,hashlib,json,shutil,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def stream_sha(stream):
 digest=hashlib.sha256()
 for chunk in iter(lambda:stream.read(1024*1024),b''):digest.update(chunk)
 return digest.hexdigest()
def sha(path):
 with path.open('rb') as stream:return stream_sha(stream)
def build(source,out):
 base=json.loads(source.with_suffix('.manifest.json').read_text(encoding='utf-8'))
 if sha(source)!=base['sha256']:raise ValueError('Source runtime32 bundle checksum mismatch')
 if out.exists():raise ValueError('Use a new delivery directory')
 out.mkdir(parents=True);stage=out/'Steam';stage.mkdir()
 with zipfile.ZipFile(source) as archive:
  for item in archive.infolist():
   name=item.filename
   if not name.startswith('Steam/') or '..' in Path(name).parts or item.is_dir():raise ValueError('Unexpected source member')
   relative=name[len('Steam/'):]
   if any(p.lower() in {'userdata','logs','config','appcache','steamapps'} or p.lower().startswith('ssfn') for p in Path(relative).parts):raise ValueError('Private user state in source')
   dest=stage/relative;dest.parent.mkdir(parents=True,exist_ok=True)
   with archive.open(item) as src,dest.open('wb') as dst:shutil.copyfileobj(src,dst,1024*1024)
   if sha(dest)!=base['filesSha256'][relative]:raise ValueError('Source member checksum mismatch')
 dll=ROOT/'entry/src/main/resources/rawfile/sdl2-x86.dll'
 if sha(dll)!='68c78590d1997122c30c992ec79857d32e5e976a0f17c0a69a2e29eab0101d70':raise ValueError('SDL2 x86 identity mismatch')
 shutil.copyfile(dll,stage/'bin/SDL2.dll')
 shutil.copyfile(ROOT/'entry/src/main/resources/rawfile/SDL2-LICENSE.txt',stage/'VPP-SDL2-LICENSE.txt')
 # steam/games is a client resource directory, not the installed-game library.
 # The original sanitizer removed it and broke post-login platform loading.
 requiredResources={'steam/games/PlatformMenu.vdf':'d95e6eee93e0c6c9badc633cbb894320cf23b1d69455a0de17f0a2c1e243914c',
  'steam/games/SteamMovie.ico':'40e48aeba8ced53fe870697359700802505159aabf150a4d163adc5595a77d90'}
 for relative,expected in requiredResources.items():
  asset=ROOT/'scripts/resources/steam'/relative
  if sha(asset)!=expected:raise ValueError('Steam client resource identity mismatch: '+relative)
  dest=stage/relative;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(asset,dest)
 notes=r"""旧柚 Pro main_proton Steam UI 配套包（runtime32）

在旧柚首页的 Steam 菜单选择“导入 Steam ZIP”，直接选本 ZIP。
新客户端统一安装到 Z:\Steam，自动生成 Steam 客户端卡片。
Steam 负责客户端登录、游戏下载、更新和鉴权；旧柚账号登录用于封面与资料同步。
默认使用 2-perf-gpu-auto 参数；兼容模式等价于 1-Normal，下一次客户端启动生效。
导入时会登记旧游戏目录中的有效 Steam 库，旧游戏文件原地保留。
再次导入作为客户端更新，保留 Z:\Steam 中的游戏、登录配置和 userdata。
更新前退出 Steam 和其游戏；切换目录时先退出原 Steam 实例。
本包保留原版 steamclient DLL 和 runtime32 下载解码辅助模块，加入官方 SDL2 x86 GPU 探测依赖。
不得把本包中的 DLL 混入旧 v3 客户端补丁。
本包保留 steam\games\PlatformMenu.vdf 客户端模块配置；该目录不属于游戏库。
本包不包含账号、登录缓存、已安装游戏和日志。
本次冷启动、游戏直达和下载闭环验证状态以外部“验证记录.md”为准。
"""
 (stage/'UI导入说明.txt').write_text(notes,encoding='utf-8-sig')
 files=sorted(p for p in stage.rglob('*') if p.is_file() and p!=stage/'SHA256SUMS.txt')
 (stage/'SHA256SUMS.txt').write_text(''.join(f'{sha(p)}  {p.relative_to(stage).as_posix()}\n' for p in files),encoding='utf-8')
 files=sorted(p for p in stage.rglob('*') if p.is_file());output=out/'Steam-main-proton-runtime32-20261011-r2.zip'
 with zipfile.ZipFile(output,'x',zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as archive:
  for p in files:
   info=zipfile.ZipInfo('Steam/'+p.relative_to(stage).as_posix(),date_time=(2026,10,11,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;info.external_attr=0o100644<<16
   with p.open('rb') as src,archive.open(info,'w') as dst:shutil.copyfileobj(src,dst,1024*1024)
 manifest=dict(name=output.name,sha256=sha(output),bytes=output.stat().st_size,unpackedBytes=sum(p.stat().st_size for p in files),
  upstreamRuntime32Sha256=base['sha256'],originalSteamclientDllsPreserved=True,clientResourcesRestored=list(requiredResources),filesSha256={p.relative_to(stage).as_posix():sha(p) for p in files},
  validation='Pending this delivery device cold-launch and game/download verification; see 验证记录.md')
 with zipfile.ZipFile(output) as archive:
  for p in files:
   with archive.open('Steam/'+p.relative_to(stage).as_posix()) as src:
    if stream_sha(src)!=manifest['filesSha256'][p.relative_to(stage).as_posix()]:raise ValueError('Output checksum mismatch')
 output.with_suffix('.manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 output.with_suffix('.sha256').write_text(f'{manifest["sha256"]}  {output.name}\n',encoding='ascii')
 print(json.dumps({k:v for k,v in manifest.items() if k!='filesSha256'},ensure_ascii=False,indent=2))
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('source',type=Path);parser.add_argument('output',type=Path)
 args=parser.parse_args();build(args.source,args.output)
