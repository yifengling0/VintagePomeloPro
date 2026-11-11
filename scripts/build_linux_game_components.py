#!/usr/bin/env python3
"""Production GNU game component entry shared by local builds and CI.
Default validates shipped immutable payloads. --rebuild requires explicit SDK
inputs in a local JSON config and reuses fixed incremental directories.
"""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
STEPS=['prepare_linux_lsof_support.py','build_steam_client_session.py','build_linux_steam_desktop.py','build_linux_steam_posix.py',
       'build_linux_steam_native_graphics.py','prepare_linux_native_repairs.py','prepare_linux_proton_dxvk.py']
def run(script,args=()):
 subprocess.run([sys.executable,str(ROOT/'scripts'/script),*map(str,args)],cwd=ROOT,check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--verify-only',action='store_true')
 p.add_argument('--rebuild',action='store_true')
 p.add_argument('--config',type=Path)
 a=p.parse_args()
 if a.rebuild:
  if a.verify_only or not a.config:p.error('--rebuild requires --config and cannot use --verify-only')
  c=json.loads(a.config.read_text());task=ROOT/'workspace_temp/hsl-sync';task.mkdir(parents=True,exist_ok=True)
  cc=c.get('cc','/usr/bin/aarch64-linux-gnu-gcc')
  run('build_steam_client_session.py',['--cc',cc])
  run('build_linux_steam_desktop.py',['--cc',cc,'--x11-deps',c['x11Deps']])
  fex=c['fex'];core=ROOT/'workspace_temp/linux-proton/native-fex-core'
  run('build_linux_native_fex.py',['--repository',ROOT/'thirdparty/fex','--output-dir',core,
      '--clang',fex['clang'],'--gnu-toolchain',fex['gnuToolchain'],'--linux-uapi',fex['linuxUapi'],
      '--download-missing','--jobs',str(c.get('jobs',8))])
  gl=c['gl'];thunks=task/'gl-thunks'
  args=['--source',core/'source','--core-receipt',core/'sigreturn-manifest.json','--output-dir',thunks,
        '--clang',fex['clang'],'--gnu-cxx',gl['gnuCxx'],'--dev-prefix',gl['devPrefix'],
        '--runtime',gl['runtime'],'--mesa-source',gl['mesaSource']]
  if gl.get('tooling'):args+=['--tooling',gl['tooling']]
  if gl.get('toolingCache'):args+=['--tooling-cache',gl['toolingCache']]
  run('build_linux_native_gl_thunks.py',args)
  raw=ROOT/'entry/src/main/resources/rawfile/linux-steam/bootstrap/native-game-support'
  run('prepare_linux_native_repairs.py',['--fex-build',core,'--gl-thunks',thunks,
      '--venus',c.get('venus',raw/'libvulkan_virtio.so'),'--software-gl',c.get('softwareGl',raw/'libsteamohos_software_gl.so')])
  run('prepare_linux_lsof_support.py',['--archive',c['lsofArchive'],'--cc',cc])
  run('build_linux_steam_posix.py',['--cc',cc])
  run('build_linux_proton_probe.py',['--cc',cc,'--output',task/'proton-shim'])
  run('prepare_linux_proton_dxvk.py',['--refresh-probe','--shim',task/'proton-shim','--network-build',c['networkBuild']])
 for script in STEPS:run(script,['--verify-only'])
 print('Verified complete production GNU game component group')
if __name__=='__main__':main()
