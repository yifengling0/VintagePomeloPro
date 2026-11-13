"""Classify visible Hiperf caller PCs using a same-process FEX perf map.

Hidden leaf PCs remain hidden: results are caller identities, not leaf CPU costs.
Overlapping specific modules with different names are reported as ambiguous.
"""
from bisect import bisect_right
from collections import Counter, defaultdict
from pathlib import Path
import argparse
import hashlib
import json
import struct


class Regions:
    def __init__(self, entries):
        events = defaultdict(list)
        for start, span, name in entries:
            assert start >= 0 and span > 0
            events[start].append((name, 1))
            events[start + span].append((name, -1))
        self.starts, self.labels = [], []
        active = Counter()
        for address in sorted(events):
            for name, delta in events[address]:
                active[name] += delta
                if not active[name]:
                    del active[name]
            specific = set(active) - {'FEXJIT'}
            if len(specific) == 1:
                label = next(iter(specific))
            elif len(specific) > 1:
                label = '<ambiguous-JIT-module>'
            elif active:
                label = '<FEX-JIT-unassigned-module>'
            else:
                label = None
            if not self.labels or label != self.labels[-1]:
                self.starts.append(address)
                self.labels.append(label)

    def lookup(self, pc):
        index = bisect_right(self.starts, pc) - 1
        return self.labels[index] if index >= 0 else None


def read_symbols(path):
    entries = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        try:
            start, span = int(fields[0], 16), int(fields[1], 16)
        except ValueError:
            continue
        if start >= 0 and span > 0:
            entries.append((start, span, fields[2]))
    assert entries, 'No valid symbol entries; do not infer diagnostic activation'
    return entries


def self_test():
    regions = Regions([(0x1000, 0x100, 'FEXJIT'), (0x1010, 0x20, 'game.exe'),
                       (0x1010, 0x20, 'game.exe'), (0x1020, 0x20, 'wined3d.dll')])
    assert regions.lookup(0xfff) is None
    assert regions.lookup(0x1000) == '<FEX-JIT-unassigned-module>'
    assert regions.lookup(0x1010) == 'game.exe'
    assert regions.lookup(0x1020) == '<ambiguous-JIT-module>'
    assert regions.lookup(0x1030) == 'wined3d.dll'
    assert regions.lookup(0x1040) == '<FEX-JIT-unassigned-module>'
    assert regions.lookup(0x1100) is None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path, nargs='?')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--jit-map', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        print('Overlap, duplicate, code-reuse conflict and boundary checks passed')
        return
    assert args.input and args.pid and args.jit_map and args.output
    symbols = read_symbols(args.jit_map)
    regions = Regions(symbols)
    data = args.input.read_bytes()
    assert data[:8] == b'PERFILE2'
    attr_offset, _, offset, size = struct.unpack_from('<QQQQ', data, 24)
    sample_type = struct.unpack_from('<Q', data, attr_offset + 24)[0]
    # DWARF adds user registers and stack after the same callchain prefix.
    # Hiperf may remove their payload after unwinding while retaining the flags.
    # Do not read trailing registers as extra callchain PCs.
    base_sample_type = 0x800103e7
    assert sample_type & base_sample_type == base_sample_type, hex(sample_type)
    assert sample_type & ~(base_sample_type | 0x3000) == 0, hex(sample_type)
    end = offset + size
    assert end <= len(data)
    maps, comm = defaultdict(list), {}
    groups = defaultdict(lambda: [0, 0])
    selected = hidden = 0
    period_total = 0
    while offset < end:
        kind, misc, length = struct.unpack_from('<IHH', data, offset)
        assert length >= 8 and offset + length <= end
        record = data[offset:offset + length]
        if kind == 3:
            _, tid = struct.unpack_from('<II', record, 8)
            comm[tid] = record[16:].split(b'\0')[0].decode(errors='replace')
        elif kind in (1, 10):
            pid, _, start, span, file_offset = struct.unpack_from('<IIQQQ', record, 8)
            name = record[40 if kind == 1 else 72:].split(b'\0')[0].decode(errors='replace')
            maps[pid].append((start, start + span, file_offset, name))
        elif kind == 9:
            _, ip, pid, tid, _, _, _, _, _, period, nr = struct.unpack_from('<QQIIQQQIIQQ', record, 8)
            assert 80 + nr * 8 <= length
            if pid == args.pid:
                selected += 1
                period_total += period
                mode = misc & 7
                source = 'visible-leaf'
                if ip == 0xffffff0000000fff:
                    hidden += 1
                    chain = struct.unpack_from('<' + 'Q' * nr, record, 80)
                    ip = next((pc for pc in chain if 0 < pc < 0xffffffffff000000
                        and pc != 0xffffff0000000fff), 0) if mode == 2 else 0
                    source = ('visible-user-callchain-PC' if ip else 'user-callchain-unavailable') if mode == 2 else 'hidden-kernel-leaf'
                name = regions.lookup(ip) if mode == 2 else None
                if name is not None:
                    name = 'FEX-region: ' + name
                else:
                    mapping = next((m for m in reversed(maps[pid]) if m[0] <= ip < m[1]), None)
                    name = mapping[3] if mapping else ('<kernel-PC-hidden>' if mode == 1 else
                        ('<user-callchain-unavailable>' if not ip else '<unassigned-user-PC>'))
                groups[(tid, mode, source, name)][0] += 1
                groups[(tid, mode, source, name)][1] += period
        offset += length
    assert offset == end and selected
    output = dict(input=str(args.input), inputSha256=hashlib.sha256(data).hexdigest(),
        symbolMap=str(args.jit_map), symbolSha256=hashlib.sha256(args.jit_map.read_bytes()).hexdigest(),
        pid=args.pid, sampleType=hex(sample_type), symbolEntries=len(symbols), selectedSamples=selected, hiddenLeafSamples=hidden,
        interpretation='Same-process perf-map required. Caller identity does not reveal the hidden execution leaf. '
            'Module conflicts stay ambiguous; map has no timestamps, so code-reuse history cannot be ordered. '
            'Period weights are sampled CPU cycles, not wall time, off-CPU wait or exact execution cost.',
        groups=[dict(tid=tid, comm=comm.get(tid), cpuMode=mode, pcSource=source, module=name,
                     samples=values[0], period=values[1], percentOfSelectedPeriod=100*values[1]/period_total)
                for (tid, mode, source, name), values in sorted(groups.items(), key=lambda row: -row[1][1])])
    args.output.write_text(json.dumps(output, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in output.items() if key != 'groups'}, indent=2))
    print(json.dumps(output['groups'][:14], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
