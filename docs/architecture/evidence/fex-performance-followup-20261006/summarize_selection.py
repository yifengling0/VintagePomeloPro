"""Report frame-batch evidence with boundary limitations stated explicitly."""
import datetime as dt
import json
import re
import statistics
from pathlib import Path

root = Path(__file__).resolve().parent
pattern = re.compile(r'^(\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}).*\[GL-PERF\].* displayed=(\d+) fps=([\d.]+)')
render = re.compile(r'^(\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}).*\[render\] ([\d.]+) fps')

def epoch(value):
    return dt.datetime.strptime('2026-'+value, '%Y-%m-%d %H:%M:%S.%f').replace(tzinfo=dt.timezone(dt.timedelta(hours=8))).timestamp()

results = {}
for event_path in root.glob('quiet-*-events.json'):
    tag = event_path.name.removesuffix('-events.json')
    data = json.loads(event_path.read_text())
    lines = (root/(tag+'-fps.txt')).read_text().splitlines()
    rows = [(epoch(m[1]), int(m[2]), float(m[3])) for line in lines if (m:=pattern.search(line))]
    lows = [(epoch(m[1]),float(m[2])) for line in lines if (m:=render.search(line))]
    windows = []
    for event in data['events']:
        start = float(event['deviceClock'].strip())
        end = float(event['loaded']['deviceClock'].strip())
        # A batch is included only when both boundaries lie in this window.
        # This can omit the first stall; do not call it end-to-end latency.
        batches = [(b[1]-a[1], b[0]-a[0], b[2]) for a,b in zip(rows,rows[1:]) if a[0]>=start and b[0]<=end]
        stat0 = event['processStat'].split(') ',1)[1].split()
        stat1 = event['loaded']['processStat'].split(') ',1)[1].split()
        windows.append(dict(character=event['character'], windowSeconds=end-start,
            completeBatches=len(batches), completeBatchFPS=sum(r[0] for r in batches)/sum(r[1] for r in batches) if batches else None,
            batchFPSMin=min((r[2] for r in batches),default=None),
            lowRenderSamples=[v for t,v in lows if start<=t<=end],
            cpuTicksDelta=sum(int(stat1[i])-int(stat0[i]) for i in [11,12])))
    results[tag]=windows
out=dict(captures=results,limitations='One OFF then ON session; no thermal match or reverse order. Host presented-frame batches only; the batch crossing each click can be omitted. No per-input latency or steady-map FPS conclusion. CPU tick deltas include all game threads.')
(root/'selection-summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
