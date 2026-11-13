import argparse
import json
import re
import automate as a
from device import ROOT,BUNDLE,shell

p=argparse.ArgumentParser(); p.add_argument('tag'); p.add_argument('--seconds',type=int,default=20); opt=p.parse_args()
assert re.fullmatch('[a-z0-9-]+',opt.tag) and 1<=opt.seconds<=45
state=shell(opt.tag+'-processes','ps -A -o PID,PPID,UID,NAME')
pids=[int(m[0]) for m in re.findall(r'^\s*(\d+)\s+\d+\s+20020279\s+(com\.vintage\.pomelopro[^\s]*)',state,re.M)]
assert pids
remote='/data/local/tmp/vp-'+opt.tag+'.data'
shell(opt.tag+'-profile','date; hiperf record -p '+','.join(map(str,pids))+' -d '+str(opt.seconds)+
    ' -f 100 -s dwarf,4096 --callchain-useronly --disable-callstack-expand --data-limit 32M -o '+remote+'; date',opt.seconds+15,'diagnostic')
path=a.receive(opt.tag,remote)
(ROOT/(opt.tag+'-identity.json')).write_text(json.dumps(dict(pids=pids,path=str(path),
    seconds=opt.seconds,frequencyHz=100,diagnosticNotFPSBenchmark=True),indent=2)+'\n')
print('Profile captured',path,flush=True)
