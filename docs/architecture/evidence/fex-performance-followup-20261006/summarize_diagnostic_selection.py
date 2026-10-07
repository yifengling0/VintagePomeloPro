import json
from pathlib import Path

root=Path(__file__).resolve().parent
out=dict(captures={},unavailableCaptures={},cumulativeSnapshots=[],limitations='Diagnostic DLL only. Consistent counters from the same Wine PID; all process threads, not CPU time. Counter snapshots can lag by about one reporting period. Cumulative snapshots cannot substitute for missing character intervals.')
for tag in ['diag-extended-cold','diag-extended-warm']:
    source=root/(tag+'-events.json')
    if not source.exists():
        out['unavailableCaptures'][tag]='The 600 live-report limit was already reached before character selection. No valid interval data was captured.'
        continue
    data=json.loads(source.read_text())
    rows=[]
    for event in data['events']:
        before=event['exactStore']; after=event['loaded']['exactStore']
        assert before['winePid']==after['winePid']
        assert after['newRows']>before['newRows'], 'Snapshot cap reached: do not report stale interval counts'
        values=[b-a for a,b in zip(before['counts'],after['counts'])]
        assert min(values)>=0 and values[0]==values[1]+values[2] and values[3]==values[4]+values[5]
        assert values[0] and values[3]
        rows.append(dict(character=event['character'],winePid=before['winePid'],
            snapshotSeconds=float(after['deviceEpoch'])-float(before['deviceEpoch']),
            f32=dict(attempt=values[0],hit=values[1],fallback=values[2],hitRatio=values[1]/values[0]),
            f64=dict(attempt=values[3],hit=values[4],fallback=values[5],hitRatio=values[4]/values[3])))
    out['captures'][tag]=rows
for name,phase in [('diag-syscall-early','early startup'),('diag-syscall-before-navigation','300-report cumulative endpoint'),('diag-extended-intro','extended-build startup'),('resume','600-report cumulative endpoint before character selection')]:
    source=json.loads((root/(name+'-stats.json')).read_text())
    assert len(source['latestConsistent'])==1
    row=source['latestConsistent'][0]
    assert row['consistent'] and not row['final']
    for key in ['f32','f64']:
        counts=row[key]
        assert counts['attempt']==counts['hit']+counts['fallback'] and counts['attempt']>0
        counts['hitRatio']=counts['hit']/counts['attempt']
    out['cumulativeSnapshots'].append(dict(source=name+'-stats.json',phase=phase,readDeviceEpoch=source['deviceEpoch'],newRows=source['newRows'],**row))
(root/'diagnostic-selection-summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
