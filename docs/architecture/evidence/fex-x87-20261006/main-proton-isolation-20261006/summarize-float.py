from pathlib import Path
import re,json,collections
root=Path(__file__).resolve().parent
def read(path):
    windows={};ops={}
    for line in path.read_text(encoding='utf-8').splitlines():
        if '[FEX-FLOAT-' not in line: continue
        d=dict(re.findall(r'(\w+)=([^ ]+)',line))
        key=(int(d['pid']),int(d['tid']),int(d['end_ns']))
        if '[FEX-FLOAT-PERF]' in line: windows[key]=d
        else: ops.setdefault(key,{})[d['op']]=d
    return windows,ops
result={}
for tag in ['float-cold','float-warm']:
    before,_=read(root/(tag+'-before.txt'))
    after,ops=read(root/(tag+'-after.txt'))
    threads={}
    for key,w in after.items():
        if key in before: continue
        tid=key[1]
        item=threads.setdefault(tid,dict(windows=0,windowSeconds=0,pc=[0]*4,rc=[0]*4,calls={},sampledNs={}))
        item['windows']+=1
        item['windowSeconds']+=(int(w['end_ns'])-int(w['begin_ns']))/1e9
        for field in ['pc','rc']:
            for i,count in enumerate(map(int,w[field].split('/'))):item[field][i]+=count
        for op,d in ops.get(key,{}).items():
            item['calls'][op]=item['calls'].get(op,0)+int(d['calls'])
            item['sampledNs'][op]=item['sampledNs'].get(op,0)+int(d['sampled_ns'])
    result[tag]=threads
(root/'float-summary.json').write_text(json.dumps(result,indent=2)+'\n')
for tag,threads in result.items():
    for tid,w in threads.items():
        total=sum(w['calls'].values())
        print(tag,tid,'windows',w['windows'],'seconds',round(w['windowSeconds'],2),
              'calls/s',round(total/w['windowSeconds']), 'PC',w['pc'],'operations',w['calls'])
