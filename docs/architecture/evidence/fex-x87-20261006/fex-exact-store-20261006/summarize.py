from pathlib import Path
import datetime as dt
import json
import re
import statistics

root=Path(__file__).resolve().parent
reports={}
for events_file in root.glob('*-events.json'):
    tag=events_file.name.removesuffix('-events.json')
    events=json.loads(events_file.read_text())
    fps_file=root/(tag+'-fps.txt')
    if not fps_file.exists(): continue
    rows=[]
    for line in fps_file.read_text(encoding='utf-8').splitlines():
        m=re.match(r'(\d\d-\d\d \d\d:\d\d:\d\d\.\d+).*\[GL-PERF\].*displayed=(\d+) fps=([\d.]+)',line)
        if m: rows.append(dict(time=m[1],displayed=int(m[2]),fps=float(m[3])))
    result=[]
    for e in events:
        # Device date is authoritative; host clock is retained for auditing.
        t=dt.datetime.strptime(e['deviceTime'].strip(),'%a %b %d %H:%M:%S CST %Y')
        chosen=[r for r in rows if t<=dt.datetime.strptime('2026-'+r['time'],'%Y-%m-%d %H:%M:%S.%f')<=t+dt.timedelta(seconds=7)]
        result.append(dict(character=e['character'],deviceStart=t.isoformat(),fps=chosen,
            minHostWindowFps=min((r['fps'] for r in chosen),default=None)))
    reports[tag]=result
out=dict(captures=reports,limitations='One session per FEX_EXACTSTORE state in the same HAP, off followed by on; no reversed repetitions or matched temperatures. Three characters added to an empty roster then removed; inspect contact sheets. Host 120-frame FPS windows overlap commands and are not input latency, GPU timing, or a main-package A/B. FEX_FLOATPERF and JIT diagnostics are disabled, but their compiled code remains; Wine winehua_perf timing is enabled equally in both states. Do not extrapolate the conversion microbenchmark to game FPS.')
(root/'comparison.json').write_text(json.dumps(out,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
for tag,items in reports.items(): print(tag,[(x['character'],x['minHostWindowFps']) for x in items])
