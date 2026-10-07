"""Attribute visible callers using a saved map from the same game process.

This deliberately does not turn hidden sample IPs into inferred leaf costs.
"""
import argparse
import bisect
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct

p = argparse.ArgumentParser()
p.add_argument('record', type=Path)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--maps', type=Path, required=True)
p.add_argument('--jit-map', type=Path)
o = p.parse_args()
maps = []
for line in o.maps.read_text().splitlines():
    m = re.match(r'^([0-9a-f]+)-([0-9a-f]+) ([-rwxps]+) ([0-9a-f]+) \S+ \d+\s*(.*)$', line)
    if m:
        lo, hi, perms, off, name = m.groups()
        maps.append((int(lo, 16), int(hi, 16), int(off, 16), name, perms))

d = o.record.read_bytes()
symbols = []
invalid_symbol_lines=0
if o.jit_map:
    for line in o.jit_map.read_text().splitlines():
        m=re.match(r'^(0x[0-9a-f]+) ([0-9a-f]+) (.+)$',line)
        if not m:
            invalid_symbol_lines+=1
            continue
        lo,size,name=m.groups()
        symbols.append((int(lo,16),int(lo,16)+int(size,16),name))
    # Exclude broad spaces and duplicate library-only entries. Prefer block
    # addresses or named dispatcher helpers over the encompassing FEXJIT map.
    symbols=[s for s in symbols if s[2].startswith('FEX_') or '+0x' in s[2]]
    symbols.sort(key=lambda s:s[0])
symbol_starts=[s[0] for s in symbols]

def lookup_symbol(ip):
    at=bisect.bisect_right(symbol_starts,ip)-1
    # A block can have multiple emitted entries. Search preceding candidates
    # for containment rather than treating the next start as a hard boundary.
    for i in range(at,max(-1,at-64),-1):
        s=symbols[i]
        if s[0]<=ip<s[1]: return s
    return None
assert d[:8] == b'PERFILE2'
attr, _, cursor, length = struct.unpack_from('<QQQQ', d, 24)
sample_type = struct.unpack_from('<Q', d, attr + 24)[0]
assert sample_type == 0x800133e7, hex(sample_type)
end = cursor + length
names = {}
processes = Counter()
threads = Counter()
callers = Counter()
pcs = Counter()
rawpcs = Counter()
total = hidden = 0
while cursor < end:
    kind, misc, size = struct.unpack_from('<IHH', d, cursor)
    assert size >= 8 and cursor + size <= end
    if kind == 3:
        _, tid = struct.unpack_from('<II', d, cursor + 8)
        names[tid] = d[cursor + 16:cursor + size].split(b'\0')[0].decode(errors='replace')
    elif kind == 9:
        _, ip, pid, tid, stamp, _, _, _, _, period, nr = struct.unpack_from('<QQIIQQQIIQQ', d, cursor + 8)
        processes[pid] += period
        if pid == o.pid:
            total += 1
            threads[tid] += period
            mode = misc & 7
            source = 'leaf'
            if ip in (0, 0xffffff0000000fff):
                hidden += 1
                chain = struct.unpack_from('<' + 'Q' * nr, d, cursor + 80)
                ip = next((pc for pc in chain if 0 < pc < 0xffffffffff000000 and pc != 0xffffff0000000fff), 0) if mode == 2 else 0
                source = 'caller' if ip else 'hidden'
            mapped = next((m for m in maps if m[0] <= ip < m[1]), None)
            label = mapped[3] if mapped and mapped[3] else '<anonymous-executable>' if mapped and 'x' in mapped[4] else '<anonymous>' if mapped else '<unmapped>' if ip else '<hidden>'
            symbol = lookup_symbol(ip)
            if symbol:
                label = symbol[2]
            # Wine maps PE executable sections anonymously. Keep the inference
            # explicit; a later PE section/layout check must validate identity.
            if not symbol and mapped and not mapped[3] and 'x' in mapped[4]:
                following = next((m for m in maps if m[0] == mapped[1]), None)
                if following and following[3].endswith('.dll'):
                    label = '<PE-code-adjacent:' + following[3].rsplit('/', 1)[-1] + '>'
            callers[(tid, mode, source, label)] += period
            if mapped:
                pcs[(tid, source, label, ip - mapped[0] + mapped[2])] += period
            elif ip:
                rawpcs[(tid, source, ip)] += period
    cursor += size
assert cursor == end and total
weight = sum(threads.values())
out = dict(pid=o.pid, recordSHA256=hashlib.sha256(d).hexdigest(), mapsSHA256=hashlib.sha256(o.maps.read_bytes()).hexdigest(),
    skippedJitMapLines=invalid_symbol_lines,
    samples=total, hiddenLeafSamples=hidden,
    limitation='Saved maps precede sampling; caller location is not leaf instruction. PE adjacency labels require binary layout verification. No off-CPU attribution.',
    processShares=[dict(pid=pid, percent=100*v/sum(processes.values())) for pid,v in processes.most_common()],
    threadShares=[dict(tid=t, name=names.get(t), percent=100*v/weight) for t,v in threads.most_common()],
    callers=[dict(tid=t, mode=m, source=s, module=n, percent=100*v/weight) for (t,m,s,n),v in callers.most_common(24)],
    pcs=[dict(tid=t, source=s, module=n, offset=hex(pc), percent=100*v/weight) for (t,s,n,pc),v in pcs.most_common(30)],
    pePCs=[dict(tid=t, source=s, module=n, offset=hex(pc), percent=100*v/weight) for (t,s,n,pc),v in pcs.most_common() if n.startswith('<PE-code-adjacent:')],
    unmappedPCs=[dict(tid=t, source=s, address=hex(pc), percent=100*v/weight) for (t,s,pc),v in rawpcs.most_common(12)])
path=o.record.with_suffix('.callers.json')
path.write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
