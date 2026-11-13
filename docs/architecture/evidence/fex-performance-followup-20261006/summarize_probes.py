from pathlib import Path
import json
import re
import statistics

root = Path(__file__).resolve().parent
def rows(path):
    text = path.read_text()
    assert 'complete=1 flags_fail=0' in text and 'bench_complete=1' in text
    return [line for line in text.splitlines() if re.match(r'^[0-3]\t', line)]
off = rows(root / 'clean-exact-off.tsv')
on = rows(root / 'clean-exact-on.tsv')
native = rows(root / 'native-exact.tsv')
assert len(off) == 48 and off == on
assert [x for x in on if x[0] in '01'] == [x for x in native if x[0] in '01']
seq = {}
for state in ('off', 'on'):
    lines = (root / ('clean-sequence-' + state + '.txt')).read_text().splitlines()
    assert len(lines) == 2
    values = []
    for line in lines:
        match = re.fullmatch(r'(f32|f64) bits=([0-9a-f]+) ie_before=(\d) ie_after=(\d) flags_before=([0-9a-f]+) flags_after=([0-9a-f]+)', line)
        assert match and match[3] == match[4] == '1' and match[5] == match[6]
        values.append(match.groups())
    seq[state] = values
assert seq['off'] == seq['on']
bench = {}
for kind in ('bench32', 'bench64'):
    bench[kind] = {state: statistics.median(float(line.split('\t')[2])
        for line in (root / ('clean-exact-' + state + '.tsv')).read_text().splitlines()
        if line.startswith(kind + '\t')) for state in ('off', 'on')}
    bench[kind]['ratioOffOn'] = bench[kind]['off'] / bench[kind]['on']
result = dict(inputCases=sum(int(line.split('\t')[3]) for line in on),
    conversions='Every input stores both F32 and F64', offOnRowsMatch=True,
    exactNormalNativeRowsMatch=True, stickyIEAndFlagsPreserved=True,
    nativeSpecialEncodingDivergentRows=sum(x != y for x, y in zip(on, native)),
    bench=bench, limitations='One fresh process per switch, OFF then ON, same clean O2/no-LTO DLL. Microbenchmark only; no game FPS conclusion. Native special-encoding differences are inherited in both switch states.')
(root / 'probe-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
