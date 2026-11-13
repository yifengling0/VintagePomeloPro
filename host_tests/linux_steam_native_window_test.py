"""Native X11/Wayland bridge contracts against a real isolated Xvfb server."""
import os, select, shutil, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 with tempfile.TemporaryDirectory(prefix='steamohos-native-window-',dir='/var/tmp') as temporary:
  task=Path(temporary);task.chmod(0o700)
  xml=Path('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
  if not xml.exists():xml=Path('/home/liufeng/steamohos-software-gl-20261019-v6/deps-root/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
  scanner=shutil.which('wayland-scanner')
  if not scanner:raise RuntimeError('wayland-scanner required for native window contract test')
  for kind,name in [('client-header','xdg-shell-client-protocol.h'),('server-header','xdg-shell-server-protocol.h'),('private-code','xdg-shell-protocol.c')]:
   subprocess.run([scanner,kind,str(xml),str(task/name)],check=True)
  subprocess.run(['cc','-fPIC','-c',str(task/'xdg-shell-protocol.c'),'-o',str(task/'protocol.o')],check=True)
  subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(task),str(ROOT/'host_tests/native_x11_window_test.cpp'),str(task/'protocol.o'),'-lwayland-client','-lwayland-server','-lxcb','-pthread','-o',str(task/'test')],check=True)
  for native in [False,True]:
   executable=task/('wsi-native' if native else 'wsi-wine')
   subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
    '-I'+str(task),'-I'+str(ROOT/'thirdparty/mesa/include'),
    *(['-DSTEAMOHOS_NATIVE_FEX_WSI=1'] if native else []),str(ROOT/'host_tests/proton_vulkan_wsi_test.cpp'),
    *([str(task/'protocol.o'),'-lxcb'] if native else []),'-lwayland-client','-ldl','-pthread','-o',str(executable)],check=True)
   subprocess.run([str(executable)],check=True,timeout=10)
  rd,wr=os.pipe();log=(task/'xvfb.log').open('w')
  server=subprocess.Popen(['Xvfb','-displayfd',str(wr),'-screen','0','800x600x24','-nolisten','unix','-listen','tcp','-ac'],pass_fds=[wr],stdout=log,stderr=log)
  os.close(wr)
  try:
   if not select.select([rd],[],[],10)[0]:raise RuntimeError('Xvfb did not become ready')
   display=os.read(rd,32).decode().strip()
   if not display.isdecimal():
    log.flush();raise RuntimeError('Invalid Xvfb display: '+(task/'xvfb.log').read_text()[-2000:])
   subprocess.run([str(task/'test')],env={**os.environ,'DISPLAY':'127.0.0.1:'+display,'XDG_RUNTIME_DIR':str(task)},timeout=20,check=True)
  finally:
   server.terminate()
   try:server.wait(timeout=5)
   except subprocess.TimeoutExpired:server.kill();server.wait(timeout=5)
   os.close(rd);log.close()
if __name__=='__main__':main()
