"""Bounded selection capture without CPU profiling or FEX counters."""
import argparse
import datetime as dt
import json
import re
import time
from PIL import Image, ImageDraw
import automate as a

p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('--host', type=int, required=True)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--stats-session', choices=['extended'])
opt = p.parse_args()
tag = opt.name
events = []

def stamp(label):
    result = dict(label=label, hostTime=dt.datetime.now().astimezone().isoformat(),
        deviceClock=a.shell(tag+'-'+label+'-clock', 'date +%s.%N'),
        processStat=a.shell(tag+'-'+label+'-stat', f'cat /proc/{opt.pid}/stat'))
    if opt.stats_session:
        text=a.shell(tag+'-'+label+'-stats', 'date +%s.%N; grep FEX_EXACTSTORE_STATS '+a.LOG)
        lines=[line for line in text.splitlines() if line.startswith('FEX_EXACTSTORE_STATS ')]
        baseline=json.loads((a.ROOT/('diag-'+opt.stats_session+'-stats-baseline.json')).read_text())['lineCount']
        latest={}
        for line in lines[baseline:]:
            m=re.search(r'pid=(\d+) final=(true|false) consistent=true f32 attempt=(\d+) hit=(\d+) fallback=(\d+) f64 attempt=(\d+) hit=(\d+) fallback=(\d+)',line)
            if m:
                values=[int(v) for v in m.groups()[2:]]
                assert values[0]==values[1]+values[2] and values[3]==values[4]+values[5]
                latest[int(m[1])]=dict(winePid=int(m[1]),counts=values)
        assert latest, 'No fresh consistent diagnostic data'
        result['exactStore']=dict(deviceEpoch=text.splitlines()[0],newRows=len(lines)-baseline,
            **max(latest.values(),key=lambda row:row['counts'][0]+row['counts'][3]))
    return result

start = stamp('start')
a.snapshot(tag+'-before')
for i, (label, x, y) in enumerate([('money', 2190, 255), ('flower', 1818, 675), ('green', 2190, 920)]):
    event = stamp(str(i)+'-begin')
    event['character'] = label
    a.input_click(tag+f'-{i}-add', x, y)
    time.sleep(3)
    a.snapshot(tag+f'-{i}-early')
    time.sleep(6)
    a.snapshot(tag+f'-{i}-loaded')
    event['loaded'] = stamp(str(i)+'-loaded')
    a.input_click(tag+f'-{i}-remove', 540, 1350)
    time.sleep(2)
    a.snapshot(tag+f'-{i}-empty')
    event['end'] = stamp(str(i)+'-end')
    events.append(event)
end = stamp('end')
a.shell(tag+'-fps', f'hilog -x -P {opt.host} -T WL_FPS,WL_EGL')
a.shell(tag+'-wine', 'date; tail -n 1600 '+a.LOG)
(a.ROOT/(tag+'-events.json')).write_text(json.dumps(dict(start=start,events=events,end=end),indent=2)+'\n')
sheet = Image.new('RGB', (960, 675), 'white')
for row, stage in enumerate(['early', 'loaded', 'empty']):
    for col in range(3):
        path = a.ROOT/(tag+f'-{col}-{stage}.png')
        im = Image.open(path); im.thumbnail((320, 200))
        x, y = col*320, row*225
        sheet.paste(im, (x, y+22))
        ImageDraw.Draw(sheet).text((x+4,y+4), path.stem, fill='black')
sheet.save(a.ROOT/(tag+'-contact.png'))
print(tag, 'capture complete', flush=True)
