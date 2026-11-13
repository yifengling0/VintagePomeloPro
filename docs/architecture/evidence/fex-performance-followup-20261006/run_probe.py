"""Fresh WOW64 process, named output, verify completion before tree teardown."""
import argparse
import json
import time
import automate as a

p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('switch', choices=['off', 'on'])
p.add_argument('--sequence', action='store_true')
p.add_argument('--stats', action='store_true')
opt = p.parse_args()
kind = 'sequence' if opt.sequence else 'exact'
remote_output = 'C:\\vp-' + opt.name + ('.txt' if opt.sequence else '.tsv')
physical = a.BASE + '/files/.wine/drive_c/' + remote_output.split('\\')[-1]
a.stop(opt.name + '-stop')
env = dict(WINEHUA_WOW64_ENGINE='fex', FEX_EXACTSTORE=str(int(opt.switch == 'on')),
    FEX_EXACTSTORE_STATS=str(int(opt.stats)), WINEHUA_WINEDEBUG='-all,+err',
    FEX_JITPERF='0', FEX_BLOCKJITNAMING='0', FEX_LIBRARYJITNAMING='0', FEX_GLOBALJITNAMING='0')
a.launch(opt.name + '-launch', 'C:\\vp-' + kind + '-store-probe.exe', env, [remote_output])
start = time.monotonic()
while time.monotonic() - start < 180:
    result = a.shell(opt.name + '-progress-' + str(int(time.monotonic() - start)),
        'if [ -f "' + physical + '" ]; then tail -n 2 "' + physical + '"; fi')
    complete = ('f64 bits=' in result) if opt.sequence else ('bench_complete=1' in result)
    if complete: break
    time.sleep(2)
else:
    a.snapshot(opt.name + '-timeout')
    a.shell(opt.name + '-timeout-log', 'tail -n 600 ' + a.LOG)
    raise RuntimeError('Probe failed to complete; evidence retained')
a.receive(opt.name, '/data/storage/el2/base/files/.wine/drive_c/' + remote_output.split('\\')[-1], True)
a.shell(opt.name + '-runtime', 'sha256sum ' + a.BASE + '/files/wine/bin/aarch64-windows/libwow64fex.dll')
a.shell(opt.name + '-log', 'date; tail -n 1600 ' + a.LOG)
print(json.dumps(dict(name=opt.name, complete=True, elapsedSeconds=time.monotonic()-start)), flush=True)
