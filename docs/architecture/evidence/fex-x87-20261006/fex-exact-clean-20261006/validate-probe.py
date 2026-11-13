from pathlib import Path
import json
import statistics

root = Path(__file__).resolve().parent
baseline = root.parent / 'fex-exact-store-20261006'
def rows(path):
    text = path.read_text()
    assert 'complete=1 flags_fail=0' in text and 'bench_complete=1' in text
    return [line for line in text.splitlines() if line[:1] in '0123' and len(line.split('\t')) == 7]
on, off = rows(root / 'exact-on.tsv'), rows(root / 'exact-off.tsv')
assert len(on) == 48 and on == off == rows(baseline / 'exact-on.tsv')
bench = {}
for kind in ['bench32', 'bench64']:
    bench[kind] = {state: statistics.median(float(line.split('\t')[2])
        for line in (root / ('exact-' + state + '.tsv')).read_text().splitlines()
        if line.startswith(kind + '\t')) for state in ['off', 'on']}
result = dict(cases=sum(int(line.split('\t')[3]) for line in on),
    offOnResultsAndIEAndFlagsMatch=True, previousCandidateResultsMatch=True,
    inheritedNoncanonicalDifferences=True, bench=bench)
(root / 'probe-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
