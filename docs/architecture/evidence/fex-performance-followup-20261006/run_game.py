import argparse
import time
import automate as a

p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('switch', choices=['off', 'on'])
p.add_argument('--stats', action='store_true')
opt = p.parse_args()
a.stop(opt.name + '-stop')
env = dict(WINEHUA_WOW64_ENGINE='fex', FEX_EXACTSTORE=str(int(opt.switch == 'on')),
    FEX_EXACTSTORE_STATS=str(int(opt.stats)), WINEHUA_WINEDEBUG='-all,+err',
    FEX_JITPERF='0', FEX_BLOCKJITNAMING='0', FEX_LIBRARYJITNAMING='0',
    FEX_GLOBALJITNAMING='0', FEX_SILENTLOG='1',
    WINEHUA_VIRGL_LOW_MAP='0', WINEHUA_DYNAMIC_BUFFER_SYSMEM='0',
    WINE_D3D_CONFIG='csmt=0x1,shader_backend=glsl')
a.shell(opt.name + '-clock', 'date')
a.launch(opt.name + '-launch', r'Z:\games\RichMan 8\RichMan 8\strat game.exe', env)
time.sleep(4)
a.snapshot(opt.name + '-initial')
a.shell(opt.name + '-runtime', 'sha256sum ' + a.BASE + '/files/wine/bin/aarch64-windows/libwow64fex.dll')
a.shell(opt.name + '-processes', 'date; ps -A -o PID,PPID,UID,NAME')
print(opt.name, 'launched', flush=True)
