from pathlib import Path
import json
import re

root = Path(__file__).resolve().parent
out = {}
for phase in ['on-cold', 'on-warm', 'off-cold', 'off-warm']:
    if not (root / (phase + '-after.txt')).exists():
        continue
    before = (root / (phase + '-before.txt')).read_text().splitlines()
    after = (root / (phase + '-after.txt')).read_text().splitlines()
    indexes = [i for i, line in enumerate(after) if line == before[-1]]
    if len(indexes) != 1:
        out[phase] = {'unavailable': 'No unique overlap anchor in bounded log tails'}
        continue
    lines = after[indexes[0] + 1:]
    metrics = {}
    records = 0
    pids = set()
    for line in lines:
        if 'scope=opengl_unix' not in line:
            continue
        records += 1
        pids.add(int(re.search(r'pid=(\d+)', line)[1]))
        for name, counts in re.findall(r'\b([a-z_]+)=([0-9]+(?:/[0-9]+){9})', line):
            values = list(map(int, counts.split('/')))
            metric = metrics.setdefault(name, dict(calls=0, totalUs=0, maxUs=0, bytes=0))
            metric['calls'] += values[0]
            metric['totalUs'] += values[1]
            metric['maxUs'] = max(metric['maxUs'], values[2])
            metric['bytes'] += values[3]
    out[phase] = dict(records=records, winePids=sorted(pids), metrics=metrics,
        limitation='Aligned log reports, not exact capture boundaries. Report windows may cross boundaries; nested metrics must not be summed as disjoint costs. No off-CPU or end-to-end input latency.')
(root / 'gl-summary.json').write_text(json.dumps(out, indent=2) + '\n')
for phase, result in out.items():
    print(phase, json.dumps(result.get('metrics', result)))
