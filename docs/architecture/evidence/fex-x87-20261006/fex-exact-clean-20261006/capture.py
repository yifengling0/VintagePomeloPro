import argparse
import concurrent.futures
import datetime as dt
import json
import time
import automate as a

p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('--pid', type=int, required=True)
p.add_argument('--host', type=int, required=True)
opt = p.parse_args()
tag = opt.name
log = '/data/app/el2/100/base/com.vintage.pomelopro/temp/wine_stderr_20261006.log'
a.shell(tag+'-before', f'date; cat /proc/{opt.pid}/stat; tail -n 1000 '+log)
a.snapshot(tag+'-before')
events=[]
remote='/data/local/tmp/vp-'+tag+'.data'
with concurrent.futures.ThreadPoolExecutor(max_workers=1) as ex:
    perf=ex.submit(a.shell, tag+'-profile',
        f'date; hiperf record -p {opt.pid} -d 32 -f 100 -s dwarf,4096 '
        f'--callchain-useronly --disable-callstack-expand --data-limit 32M -o {remote}; date', 45)
    time.sleep(2)
    for i,(label,x,y) in enumerate([('money',2190,255),('flower',1818,675),('green',2190,920)]):
        events.append(dict(character=label, requestedAt=dt.datetime.now().astimezone().isoformat(),
            deviceTime=a.shell(tag+f'-{i}-clock', 'date')))
        a.shell(tag+f'-{i}-add', f'uinput -T -c {x} {y} 60', policy='automation')
        time.sleep(4)
        a.snapshot(tag+f'-{i}-loaded')
        a.shell(tag+f'-{i}-remove', 'uinput -T -c 540 1350 60', policy='automation')
        time.sleep(1)
        a.snapshot(tag+f'-{i}-empty')
    result=perf.result()
    assert 'Sample lost: 0' in result and 'Sample records:' in result
a.receive(tag,remote)
a.shell(tag+'-after', 'date; tail -n 1200 '+log)
a.shell(tag+'-fps', f'hilog -x -P {opt.host} -T WL_FPS,WL_EGL')
(a.ROOT/(tag+'-events.json')).write_text(json.dumps(events,indent=2)+'\n',encoding='utf-8')
from PIL import Image, ImageOps, ImageDraw
paths=list(a.ROOT.glob(tag+'-?-loaded.png'))+list(a.ROOT.glob(tag+'-?-empty.png'))
sheet=Image.new('RGB',(960,630),'white')
for i,path in enumerate(paths):
    im=Image.open(path)
    im.thumbnail((320,200))
    x,y=i%3*320,i//3*315
    sheet.paste(im,(x,y+25))
    ImageDraw.Draw(sheet).text((x+5,y+5),path.stem,fill='black')
sheet.save(a.ROOT/(tag+'-contact.png'))
