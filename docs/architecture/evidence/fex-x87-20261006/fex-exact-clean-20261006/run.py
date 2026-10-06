import argparse,time
import automate as a
p=argparse.ArgumentParser()
p.add_argument('kind',choices=['exact-probe','cpu-probe','game'])
p.add_argument('switch',choices=['off','on'])
p.add_argument('--diag',action='store_true')
opt=p.parse_args()
tag=opt.kind+'-'+opt.switch+('-diag' if opt.diag else '')
a.shell(tag+'-stop','aa force-stop '+a.BUNDLE,policy='automation')
a.shell(tag+'-clock','date')
env=dict(WINEHUA_WOW64_ENGINE='fex',FEX_EXACTSTORE='1' if opt.switch=='on' else '0',
    FEX_FLOATPERF='1' if opt.diag else '0',FEX_JITPERF='0',FEX_BLOCKJITNAMING='0',
    FEX_LIBRARYJITNAMING='0',FEX_GLOBALJITNAMING='0',FEX_JITMAPDIR='',FEX_SILENTLOG='0',
    WINEHUA_WINEDEBUG='-all,+err',WINEDEBUG='-all,+err',WINEHUA_VIRGL_LOW_MAP='0',
    WINEHUA_DYNAMIC_BUFFER_SYSMEM='0',WINE_D3D_CONFIG='csmt=0x1,shader_backend=glsl')
if opt.kind=='game':env['WINEHUA_WINEDEBUG']='-all,+err,+winehua_perf'
exe={'exact-probe':r'C:\vp-exact-store-probe.exe','cpu-probe':r'C:\vp32-route-probe.exe',
    'game':r'Z:\games\RichMan 8\RichMan 8\strat game.exe'}[opt.kind]
a.launch(tag+'-launch',exe,env)
time.sleep(4)
a.snapshot(tag+'-initial')
a.shell(tag+'-identity','date; sha256sum /data/app/el2/100/base/com.vintage.pomelopro/files/wine/bin/aarch64-windows/libwow64fex.dll; ps -A -o PID,PPID,UID,NAME')
