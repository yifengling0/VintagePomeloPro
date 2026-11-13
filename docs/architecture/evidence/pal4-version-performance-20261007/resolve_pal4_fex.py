import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

r = Path(__file__).resolve().parent
dll = r / 'pal4-fex-clean-unstripped.dll'
binary = dll.read_bytes()
assert hashlib.sha256(binary).hexdigest() == '3c1bb5f807a4429e2e44690c10b2d3e367c6dfdaee2e999a5fd472d59d75aaf4'
pe = struct.unpack_from('<I',binary,60)[0]
assert binary[pe:pe+4] == b'PE\0\0'
section_count = struct.unpack_from('<H',binary,pe+6)[0]
optional_size = struct.unpack_from('<H',binary,pe+20)[0]
base = struct.unpack_from('<Q',binary,pe+48)[0]
sections = []
for i in range(section_count):
    at = pe+24+optional_size+i*40
    name=binary[at:at+8].rstrip(b'\0').decode()
    size,rva,raw_size,raw=struct.unpack_from('<IIII',binary,at+8)
    sections.append(dict(name=name,rva=hex(rva),size=hex(size),rawOffset=hex(raw)))
symbols=[]
for line in (r/'pal4-fex-symbols.txt').read_text().splitlines():
    m=re.match(r'^([0-9a-f]+) [tT] (.*)$',line)
    if m: symbols.append((int(m[1],16)-base,m[2]))
symbols.sort()
addresses=[s[0] for s in symbols]
profile=json.loads((r/'pal4-145-fex-load.callers.json').read_text())
totals=Counter()
threads=Counter()
for point in profile['pePCs']:
    if point['module']!='<PE-code-adjacent:libwow64fex.dll>': continue
    address=int(point['offset'],16)
    at=bisect.bisect_right(addresses,address)-1
    assert at>=0
    name=symbols[at][1]
    totals[name]+=point['percent']
    threads[(point['tid'],name)]+=point['percent']
out=dict(binarySHA256=hashlib.sha256(binary).hexdigest(),imageBase=hex(base),sections=sections,
    anonymousMappedBase='0x6ffc430000',nextMappedRVA='0x1c1000',
    limitation='Symbolized visible callers, not hidden leaf instructions or measured helper self-time.',
    symbols=[dict(symbol=s,callerCycleWeightPercent=v) for s,v in totals.most_common(18)],
    threadSymbols=[dict(tid=t,symbol=s,callerCycleWeightPercent=v) for (t,s),v in threads.most_common(24)])
(r/'pal4-fex-symbol-attribution.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
