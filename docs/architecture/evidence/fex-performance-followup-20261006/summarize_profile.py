"""Classify a bounded Hiperf record without inventing hidden leaf symbols."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct

p=argparse.ArgumentParser(); p.add_argument('input', type=Path); p.add_argument('--pid',type=int,required=True)
opt=p.parse_args(); data=opt.input.read_bytes(); assert data[:8]==b'PERFILE2'
attr, _, offset, size=struct.unpack_from('<QQQQ',data,24)
sample_type=struct.unpack_from('<Q',data,attr+24)[0]
base=0x800103e7
assert sample_type & base == base and not sample_type & ~(base | 0x3000), hex(sample_type)
end=offset+size; maps=defaultdict(list); names={}; groups=Counter(); pcs=Counter(); total=hidden=0
while offset<end:
    kind,misc,length=struct.unpack_from('<IHH',data,offset); assert length>=8 and offset+length<=end
    if kind==3:
        _,tid=struct.unpack_from('<II',data,offset+8)
        names[tid]=data[offset+16:offset+length].split(b'\0')[0].decode(errors='replace')
    elif kind in (1,10):
        pid,_,start,span,fileoff=struct.unpack_from('<IIQQQ',data,offset+8)
        name=data[offset+(40 if kind==1 else 72):offset+length].split(b'\0')[0].decode(errors='replace')
        maps[pid].append((start,start+span,fileoff,name))
    elif kind==9:
        _,ip,pid,tid,stamp,_,_,_,_,period,nr=struct.unpack_from('<QQIIQQQIIQQ',data,offset+8)
        assert 80+nr*8<=length
        if pid==opt.pid:
            total+=1; mode=misc&7; source='leaf'
            if ip in (0,0xffffff0000000fff):
                hidden+=1
                chain=struct.unpack_from('<'+'Q'*nr,data,offset+80)
                ip=next((pc for pc in chain if 0<pc<0xffffffffff000000 and pc!=0xffffff0000000fff),0) if mode==2 else 0
                source='caller' if ip else 'unavailable'
            m=next((m for m in reversed(maps[pid]) if m[0]<=ip<m[1]),None)
            module=m[3] if m else ('<kernel-hidden>' if mode==1 else '<user-unassigned>' if ip else '<user-unavailable>')
            groups[(tid,mode,source,module)]+=period
            if m: pcs[(tid,source,module,ip-m[0]+m[2])]+=period
    offset+=length
assert offset==end and total
weight=sum(groups.values())
out=dict(pid=opt.pid,inputSHA256=hashlib.sha256(data).hexdigest(),sampleType=hex(sample_type),samples=total,hiddenLeafSamples=hidden,
    limitation='Caller identities and cycle weights only. Hidden leaf PCs, off-CPU waits, helper costs and guest instructions cannot be resolved from this capture.',
    groups=[dict(tid=t,thread=names.get(t),mode=m,source=s,module=n,percent=100*v/weight) for (t,m,s,n),v in groups.most_common(16)],
    visiblePCs=[dict(tid=t,source=s,module=n,fileOffset=hex(pc),percent=100*v/weight) for (t,s,n,pc),v in pcs.most_common(20)])
opt.input.with_suffix('.summary.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
