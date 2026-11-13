"""Read exact-store counters after an explicit log offset; no old process reuse."""
import argparse
import json
import re
import automate as a

p=argparse.ArgumentParser(); p.add_argument('name'); p.add_argument('--baseline',action='store_true')
p.add_argument('--session', default='syscall', choices=['syscall','extended'])
opt=p.parse_args()
text=a.shell(opt.name+'-stats', 'date +%s.%N; grep FEX_EXACTSTORE_STATS '+a.LOG)
lines=[line for line in text.splitlines() if line.startswith('FEX_EXACTSTORE_STATS ')]
base=a.ROOT/('diag-'+opt.session+'-stats-baseline.json')
if opt.baseline:
    base.write_text(json.dumps(dict(lineCount=len(lines)))+'\n')
    print('stats baseline lines',len(lines))
else:
    count=json.loads(base.read_text())['lineCount']
    pattern=re.compile(r'pid=(\d+) final=(true|false) consistent=(true|false) f32 attempt=(\d+) hit=(\d+) fallback=(\d+) f64 attempt=(\d+) hit=(\d+) fallback=(\d+)')
    rows=[]
    for line in lines[count:]:
        m=pattern.search(line); assert m, line
        nums=[int(v) for v in m.groups()[3:]]
        row=dict(pid=int(m[1]),final=m[2]=='true',consistent=m[3]=='true',
            f32=dict(zip(['attempt','hit','fallback'],nums[:3])),f64=dict(zip(['attempt','hit','fallback'],nums[3:])))
        if row['consistent']:
            assert all(row[k]['attempt']==row[k]['hit']+row[k]['fallback'] for k in ['f32','f64'])
        rows.append(row)
    latest={}
    for row in rows:
        if row['consistent']: latest[row['pid']]=row
    out=dict(deviceEpoch=text.splitlines()[0], newRows=len(rows), latestConsistent=list(latest.values()))
    (a.ROOT/(opt.name+'-stats.json')).write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps(out,indent=2))
