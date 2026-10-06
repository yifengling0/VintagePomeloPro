"""Inspect older, matched JIT maps for guest callers behind x87 ABI frames."""
from collections import defaultdict
from pathlib import Path
import hashlib
import importlib.util
import json
import struct

root = Path(__file__).resolve().parent
evidence = root.parent / 'fex-softfloat-lto-20261006'
module_path = root.parent / 'fex-jit-symbols-20261006/symbolize_jit_profile.py'
spec = importlib.util.spec_from_file_location('jit_regions', module_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
all_results = {}
for phase in ['cold', 'warm']:
    source = evidence / ('avatars-' + phase + '-load.data')
    symbols = evidence / ('avatars-' + phase + '-load-symbols.map')
    regions = module.Regions(module.read_symbols(symbols))
    data = source.read_bytes()
    attr, _, offset, size = struct.unpack_from('<QQQQ', data, 24)
    sample_type = struct.unpack_from('<Q', data, attr + 24)[0]
    assert sample_type == 0x800133e7
    end = offset + size
    comm = {}
    groups = defaultdict(lambda: [0, 0])
    examples = {}
    while offset < end:
        kind, misc, length = struct.unpack_from('<IHH', data, offset)
        assert length >= 8 and offset + length <= end
        record = data[offset:offset + length]
        if kind == 3:
            _, tid = struct.unpack_from('<II', record, 8)
            comm[tid] = record[16:].split(b'\0')[0].decode(errors='replace')
        elif kind == 9:
            _, ip, pid, tid, _, _, _, _, _, period, nr = struct.unpack_from('<QQIIQQQIIQQ', record, 8)
            assert 80 + 8 * nr <= length
            if pid == 4062 and misc & 7 == 2:
                chain = struct.unpack_from('<' + 'Q' * nr, record, 80)
                labels = [(hex(pc), regions.lookup(pc)) for pc in chain
                          if 0 < pc < 0xffffffffff000000 and pc != 0xffffff0000000fff]
                abi = next((i for i, (_, label) in enumerate(labels)
                            if label and label.startswith('FEX_ABI_') and 'F80' in label), None)
                if abi is not None:
                    callers = [label for _, label in labels[abi + 1:]
                               if label and not label.startswith(('FEX', '<'))]
                    caller = callers[0] if callers else '<guest-caller-not-visible>'
                    key = tid, labels[abi][1], caller
                    groups[key][0] += 1
                    groups[key][1] += period
                    examples.setdefault(key, labels[:10])
        offset += length
    assert offset == end
    all_results[phase] = dict(inputSha256=hashlib.sha256(data).hexdigest(),
        symbolSha256=hashlib.sha256(symbols.read_bytes()).hexdigest(),
        groups=[dict(tid=k[0], thread=comm.get(k[0]), abi=k[1], nearestGuestCaller=k[2],
                     samples=v[0], sampledCycles=v[1], example=examples[k])
                for k, v in sorted(groups.items(), key=lambda item: -item[1][1])])
result = dict(limitation='Historical PID 4062 only, matching same-process maps. '
    'Visible stack association, not hidden leaf cost or wall time. Cold/warm first avatar was invalid '
    'in these old captures; no whole-pass FPS conclusion. Missing callers remain unknown.', captures=all_results)
(root / 'fallback-caller-attribution.json').write_text(json.dumps(result, indent=2) + '\n')
for phase, info in all_results.items():
    print(phase)
    for row in info['groups'][:12]:
        print(row['tid'], row['thread'], row['abi'], row['nearestGuestCaller'], row['samples'])
