#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Bounded PE inventory including the *applied* ARM64X EC image view.

Format references (independent Python implementation; no external PE library):
- Microsoft PE/COFF specification and IMAGE_LOAD_CONFIG_DIRECTORY64.
- LLVM Object/COFF.h: CHPE and dynamic relocation layouts.
- LLVM COFFObjectFile.cpp: getHybridObjectView / Arm64XRelocRef semantics.
https://llvm.org/doxygen/COFFObjectFile_8cpp_source.html
https://learn.microsoft.com/en-us/windows/win32/debug/pe-format

Static inventory proves neither Wine/FEX loading nor correct rendered pixels.
Unknown relocation kinds or ambiguous/unbacked RVAs are errors, not success.
"""
import hashlib
import re
import struct

MAX_FILE = 256 << 20
MAX_IMAGE = 512 << 20
MAX_SYMBOLS = 100000
MAX_FIXUPS = 1000000
EXPECTED_EXPORTS = {
    'dxgi.dll': {'CreateDXGIFactory': 9, 'CreateDXGIFactory1': 10,
                 'CreateDXGIFactory2': 11, 'DXGIDeclareAdapterRemovalSupport': 16,
                 'DXGIGetDebugInterface1': 17},
    'd3d11.dll': {'D3D11CoreCreateDevice': 18, 'D3D11CreateDevice': 22,
                  'D3D11CreateDeviceAndSwapChain': 23, 'D3D11On12CreateDevice': 24},
}


def _need(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('PE byte range outside file')


def _u(data, offset, size):
    _need(data, offset, size)
    return int.from_bytes(data[offset:offset+size], 'little')


class _PE:
    def __init__(self, data):
        if not isinstance(data, (bytes, bytearray)) or not 64 <= len(data) <= MAX_FILE:
            raise ValueError('Invalid PE file size/type')
        self.data = data
        if data[:2] != b'MZ':
            raise ValueError('Missing MZ signature')
        self.pe = _u(data, 60, 4)
        _need(data, self.pe, 24)
        if self.pe < 64 or data[self.pe:self.pe+4] != b'PE\0\0':
            raise ValueError('Missing PE signature')
        self.machine = _u(data, self.pe+4, 2)
        count = _u(data, self.pe+6, 2)
        self.characteristics = _u(data, self.pe+22, 2)
        optional_bytes = _u(data, self.pe+20, 2)
        self.opt = self.pe+24
        _need(data, self.opt, optional_bytes)
        self.magic = _u(data, self.opt, 2)
        if self.magic not in (0x10b, 0x20b) or not 1 <= count <= 96:
            raise ValueError('Invalid PE optional header/section count')
        self.is64 = self.magic == 0x20b
        self.pointer_size = 8 if self.is64 else 4
        relative_dirs = 112 if self.is64 else 96
        if optional_bytes < relative_dirs:
            raise ValueError('Truncated PE optional header')
        self.dirs = self.opt+relative_dirs
        self.directory_count = _u(data, self.dirs-4, 4)
        if self.directory_count > 16 or self.directory_count*8 > optional_bytes-relative_dirs:
            raise ValueError('Invalid PE data-directory count')
        self.image_base = _u(data, self.opt+(24 if self.is64 else 28), self.pointer_size)
        self.image_size = _u(data, self.opt+56, 4)
        self.header_size = _u(data, self.opt+60, 4)
        section_table = self.opt+optional_bytes
        if not section_table+40*count <= self.header_size <= len(data) or not self.header_size <= self.image_size <= MAX_IMAGE:
            raise ValueError('Invalid PE image/header size')
        self.sections = []
        raw_ranges = []
        virtual_ranges = []
        for index in range(count):
            at = section_table+40*index
            _need(data, at, 40)
            virtual_size, rva, size, offset = struct.unpack_from('<IIII', data, at+8)
            _need(data, offset, size)
            extent = max(virtual_size, size)
            if extent and (rva < self.header_size or rva+extent > self.image_size):
                raise ValueError('Invalid PE section RVA range')
            if size:
                if offset < self.header_size:
                    raise ValueError('PE section overlaps headers')
                raw_ranges.append((offset, offset+size))
            if extent:
                virtual_ranges.append((rva, rva+extent))
            self.sections.append({'name': bytes(data[at:at+8]).rstrip(b'\0').decode('ascii', 'replace'),
                                  'rva': rva, 'virtualSize': virtual_size,
                                  'rawOffset': offset, 'rawSize': size,
                                  'characteristics': _u(data, at+36, 4)})
        for ranges in (raw_ranges, virtual_ranges):
            ordered = sorted(ranges)
            if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
                raise ValueError('Overlapping PE sections')

    def off(self, rva, size=1):
        if rva < 0 or size < 0 or rva+size > self.image_size:
            raise ValueError('Invalid PE RVA range')
        if rva < self.header_size:
            if rva+size > self.header_size:
                raise ValueError('PE RVA crosses headers')
            _need(self.data, rva, size)
            return rva
        matches = [s['rawOffset']+rva-s['rva'] for s in self.sections
                   if s['rva'] <= rva and rva-s['rva']+size <= s['rawSize']]
        if len(matches) != 1:
            raise ValueError('Unmapped/ambiguous PE RVA '+hex(rva))
        _need(self.data, matches[0], size)
        return matches[0]

    def value(self, rva, size):
        return _u(self.data, self.off(rva, size), size)

    def string(self, rva, limit=4096):
        offset = self.off(rva)
        end = self.data.find(b'\0', offset, min(len(self.data), offset+limit))
        if end < 0:
            raise ValueError('Unterminated PE string')
        self.off(rva, end-offset+1)
        try:
            return bytes(self.data[offset:end]).decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('Non-ASCII PE symbol/module name') from error

    def directory(self, index):
        if index >= self.directory_count:
            return 0, 0
        rva, size = struct.unpack_from('<II', self.data, self.dirs+8*index)
        if bool(rva) != bool(size):
            raise ValueError('Inconsistent PE directory address/size')
        if rva and index != 4:  # The certificate table uses file offsets.
            self.off(rva, size)
        return rva, size

    def imports(self, delay=False):
        rva, length = self.directory(13 if delay else 1)
        if not rva:
            return []
        stride = 32 if delay else 20
        rows = []
        total_symbols = 0
        for index in range(min(length//stride, 4096)):
            offset = self.off(rva+stride*index, stride)
            values = struct.unpack_from('<'+('I'*8 if delay else 'I'*5), self.data, offset)
            if not any(values):
                return rows
            if delay:
                attributes, name, _, iat, names, _, _, _ = values
                if attributes not in (0, 1):
                    raise ValueError('Unsupported delay import attributes')
                if not attributes:
                    name -= self.image_base
                    iat -= self.image_base
                    names = names-self.image_base if names else 0
            else:
                names, _, _, name, iat = values
            module = self.string(name, 512)
            if not re.fullmatch(r'[A-Za-z0-9_.+-]+\.dll', module, re.I):
                raise ValueError('Unsafe PE import module name')
            thunk = names or iat
            if not thunk:
                raise ValueError('Missing import name/IAT table')
            symbols = []
            for number in range(MAX_SYMBOLS-total_symbols):
                value = self.value(thunk+number*self.pointer_size, self.pointer_size)
                if not value:
                    break
                ordinal_bit = 1 << (self.pointer_size*8-1)
                if value & ordinal_bit:
                    if value & ~(ordinal_bit | 0xffff):
                        raise ValueError('Malformed ordinal import')
                    symbols.append({'ordinal': value & 0xffff})
                else:
                    symbols.append({'name': self.string(value+2), 'hint': self.value(value, 2)})
            else:
                raise ValueError('PE import symbol limit/unterminated table')
            total_symbols += len(symbols)
            rows.append({'module': module, 'symbols': symbols})
        raise ValueError('PE import descriptor limit/unterminated directory')

    def exports(self):
        rva, length = self.directory(0)
        if not rva:
            return []
        if length < 40:
            raise ValueError('Truncated export directory')
        offset = self.off(rva, 40)
        base, functions, names, function_table, name_table, ordinal_table = struct.unpack_from('<IIIIII', self.data, offset+16)
        if functions > MAX_SYMBOLS or names > MAX_SYMBOLS:
            raise ValueError('PE export symbol limit')
        self.off(function_table, functions*4)
        if names:
            self.off(name_table, names*4)
            self.off(ordinal_table, names*2)
        named = {}
        seen = set()
        for index in range(names):
            name = self.string(self.value(name_table+4*index, 4))
            ordinal_index = self.value(ordinal_table+2*index, 2)
            if not name or name in seen or ordinal_index >= functions:
                raise ValueError('Duplicate/invalid export name or ordinal')
            seen.add(name)
            named.setdefault(ordinal_index, []).append(name)
        rows = []
        for index in range(functions):
            target = self.value(function_table+index*4, 4)
            if not target:
                if index in named:
                    raise ValueError('Named export points at empty ordinal')
                continue
            self.off(target)
            for name in named.get(index, [None]):
                row = {'name': name, 'ordinal': base+index, 'rva': target}
                if rva <= target < rva+length:
                    forwarder = self.string(target)
                    if '.' not in forwarder or target+len(forwarder)+1 > rva+length:
                        raise ValueError('Invalid export forwarder')
                    row['forwarder'] = forwarder
                rows.append(row)
        return rows

    def load_config(self):
        rva, size = self.directory(10)
        if not rva:
            return {'present': False}
        declared = self.value(rva, 4)
        if declared < 4:
            raise ValueError('Invalid load config size')
        available = min(size, declared)
        result = {'present': True, 'rva': rva, 'directorySize': size, 'declaredSize': declared}
        fields = {'chpePointer': (200, 8) if self.is64 else (124, 4),
                  'dynamicPointer': (192, 8) if self.is64 else (120, 4),
                  'dynamicOffset': (224, 4) if self.is64 else (136, 4),
                  'dynamicSection': (228, 2) if self.is64 else (140, 2)}
        for name, (offset, width) in fields.items():
            result[name] = self.value(rva+offset, width) if available >= offset+width else 0
        return result

    def chpe(self):
        config = self.load_config()
        pointer = config.get('chpePointer', 0)
        if not pointer:
            return {'present': False}
        if not self.is64 or self.machine not in (0xaa64, 0x8664):
            raise ValueError('CHPE metadata requires ARM64/AMD64 PE32+ image')
        rva = pointer-self.image_base
        version = self.value(rva, 4)
        if version not in (1, 2):
            raise ValueError('Unsupported CHPE metadata version')
        count = 23 if version == 2 else 20
        at = self.off(rva, count*4)
        fields = struct.unpack_from('<'+'I'*count, self.data, at)
        code_map, code_count, ranges, redirects = fields[1:5]
        range_count, redirect_count = fields[12:14]
        if any(n > MAX_SYMBOLS for n in (code_count, range_count, redirect_count)):
            raise ValueError('CHPE table count limit')
        code_ranges = []
        if code_count:
            self.off(code_map, code_count*8)
        for index in range(code_count):
            start = self.value(code_map+index*8, 4)
            length = self.value(code_map+index*8+4, 4)
            kind, start = start & 3, start & ~3
            if kind == 3 or not length:
                raise ValueError('Invalid CHPE code range')
            # Code-map classifications may span aligned gaps between executable
            # sections (LLVM combines its x64 range with .hexpthk). They are
            # image-address ranges, not a contiguous file-backed byte slice.
            if start < self.header_size or start+length > self.image_size:
                raise ValueError('CHPE code classification outside image')
            code_ranges.append({'rva': start, 'bytes': length, 'type': ('arm64', 'arm64ec', 'amd64')[kind]})
        ordered = sorted((r['rva'], r['rva']+r['bytes']) for r in code_ranges)
        if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
            raise ValueError('Overlapping CHPE code classifications')
        if range_count:
            self.off(ranges, range_count*12)
        for index in range(range_count):
            start, end, entry = struct.unpack_from('<III', self.data, self.off(ranges+12*index, 12))
            if start >= end:
                raise ValueError('Invalid CHPE entry point range')
            if start < self.header_size or end > self.image_size:
                raise ValueError('CHPE entry point range outside image')
            self.off(entry)
        if redirect_count:
            self.off(redirects, redirect_count*8)
        for index in range(redirect_count):
            source, destination = struct.unpack_from('<II', self.data, self.off(redirects+8*index, 8))
            self.off(source)
            self.off(destination)
        return {'present': True, 'pointer': pointer, 'rva': rva, 'version': version,
                'codeRanges': code_ranges, 'entryPointRangeCount': range_count,
                'redirectionCount': redirect_count,
                'metadataSha256': hashlib.sha256(bytes(self.data[at:at+count*4])).hexdigest()}

    def loaded_header(self):
        config = self.load_config()
        chpe = self.chpe()
        export_rva, export_size = self.directory(0)
        import_rva, import_size = self.directory(1)
        return {'machine': self.machine, 'optionalMagic': self.magic,
                'timeDateStamp': _u(self.data, self.pe+8, 4), 'sizeOfImage': self.image_size,
                'loadConfigRva': config.get('rva', 0),
                'loadConfigSize': config.get('directorySize', 0),
                'chpeMetadataRva': chpe.get('rva', 0),
                'chpeMetadataVersion': chpe.get('version', 0),
                'exportDirectoryRva': export_rva, 'exportDirectorySize': export_size,
                'importDirectoryRva': import_rva, 'importDirectorySize': import_size}

    def graph(self):
        export_rva, _ = self.directory(0)
        export_name = self.value(export_rva+12, 4) if export_rva else 0
        imports = self.imports()
        delayed = self.imports(delay=True)
        return {'machine': self.machine, 'machineHex': hex(self.machine), 'optionalMagic': self.magic,
                'dll': bool(self.characteristics & 0x2000), 'chpe': self.chpe(),
                'loadedHeader': self.loaded_header(),
                'exportDllName': self.string(export_name) if export_name else None,
                'imports': imports, 'delayImports': delayed, 'exports': self.exports(),
                'importModules': sorted({r['module'].lower() for r in imports+delayed})}


def _hybrid_view(pe):
    """Apply only documented ARM64X fixups to an independent file-byte view."""
    config = pe.load_config()
    section = config.get('dynamicSection', 0)
    pointer = config.get('dynamicPointer', 0)
    if not section and not pointer:
        return None, {'present': False, 'fixups': 0}
    if section:
        if section > len(pe.sections):
            raise ValueError('Invalid dynamic relocation section index')
        sec = pe.sections[section-1]
        offset = config['dynamicOffset']
        if offset+8 > sec['rawSize']:
            raise ValueError('Dynamic relocation section offset outside file')
        rva = sec['rva']+offset
        available = sec['rawSize']-offset
        if pointer and pointer-pe.image_base != rva:
            raise ValueError('Conflicting dynamic relocation locations')
    else:
        rva = pointer-pe.image_base
        available = len(pe.data)-pe.off(rva)
    at = pe.off(rva, 8)
    version, size = struct.unpack_from('<II', pe.data, at)
    if version not in (1, 2) or size+8 > available:
        raise ValueError('Unsupported/truncated dynamic relocation table')
    pe.off(rva, size+8)
    end = at+8+size
    cursor = at+8
    operations = []
    entries = []
    while cursor < end:
        minimum = (24 if pe.is64 else 20) if version == 2 else (12 if pe.is64 else 8)
        if cursor+minimum > end:
            raise ValueError('Truncated dynamic relocation entry')
        if version == 1:
            symbol = _u(pe.data, cursor, pe.pointer_size)
            payload_size = _u(pe.data, cursor+pe.pointer_size, 4)
            header_size = minimum
        else:
            header_size = _u(pe.data, cursor, 4)
            payload_size = _u(pe.data, cursor+4, 4)
            symbol = _u(pe.data, cursor+8, pe.pointer_size)
            group = _u(pe.data, cursor+8+pe.pointer_size, 4)
            flags = _u(pe.data, cursor+12+pe.pointer_size, 4)
            if group or flags:
                raise ValueError('Unsupported dynamic relocation v2 group/flags')
        if header_size < minimum or cursor+header_size+payload_size > end:
            raise ValueError('Dynamic relocation payload outside table')
        if symbol != 6:
            raise ValueError('Unsupported dynamic relocation symbol '+str(symbol))
        block = cursor+header_size
        payload_end = block+payload_size
        entries.append({'symbol': symbol, 'bytes': payload_size})
        while block < payload_end:
            if block+8 > payload_end:
                raise ValueError('Truncated ARM64X relocation block')
            page, block_size = struct.unpack_from('<II', pe.data, block)
            if page & 0xfff or block_size <= 8 or block_size % 4 or block+block_size > payload_end:
                raise ValueError('Invalid ARM64X relocation block')
            position = block+8
            while position < block+block_size:
                encoded = _u(pe.data, position, 2)
                position += 2
                if encoded == 0:
                    if position != block+block_size:
                        raise ValueError('Unexpected ARM64X relocation terminator')
                    break
                kind, arg, target = (encoded >> 12) & 3, encoded >> 14, page+(encoded & 0xfff)
                width = 4 if kind == 2 else 1 << arg
                if kind == 3 or kind == 1 and not arg or target % width:
                    raise ValueError('Invalid ARM64X relocation type/size/alignment')
                if kind == 0:
                    value = 0
                elif kind == 1:
                    if position+width > block+block_size:
                        raise ValueError('Truncated ARM64X value fixup')
                    value = _u(pe.data, position, width)
                    position += width
                else:
                    if position+2 > block+block_size:
                        raise ValueError('Truncated ARM64X delta fixup')
                    value = _u(pe.data, position, 2) * (8 if arg & 2 else 4)
                    if arg & 1:
                        value = -value
                    position += 2
                target_offset = pe.off(target, width)
                operations.append((target_offset, width, kind, value))
                if len(operations) > MAX_FIXUPS:
                    raise ValueError('ARM64X fixup count limit')
            block += block_size
        cursor = payload_end
    intervals = sorted((offset, offset+width) for offset, width, _, _ in operations)
    if any(a[1] > b[0] for a, b in zip(intervals, intervals[1:])):
        raise ValueError('Overlapping ARM64X fixups')
    if not operations:
        raise ValueError('Empty ARM64X fixup table')
    derived = bytearray(pe.data)
    for offset, width, kind, value in operations:
        if kind == 2:
            value = (_u(derived, offset, width)+value) & 0xffffffff
        derived[offset:offset+width] = value.to_bytes(width, 'little')
    return derived, {'present': True, 'version': version, 'rva': rva, 'bytes': size+8,
                     'entries': entries, 'fixups': len(operations),
                     'types': {str(k): sum(1 for _, _, kind, _ in operations if kind == k) for k in (0, 1, 2)},
                     'derivedViewSha256': hashlib.sha256(derived).hexdigest()}


def inspect_pe(data):
    """Return JSON-safe static inventory; malformed/unknown hybrid data raises."""
    original = _PE(data)
    primary = original.graph()
    view, dynamic = _hybrid_view(original)
    graphs = {'primary': primary}
    architecture = {0x14c: 'i386', 0x8664: 'x86_64', 0xaa64: 'aarch64'}.get(original.machine, 'unknown')
    if primary['chpe']['present']:
        architecture = 'arm64ec' if original.machine == 0x8664 else 'arm64x-incomplete'
    if view is not None:
        if original.machine != 0xaa64 or not primary['chpe']['present']:
            raise ValueError('ARM64X fixups require native ARM64 CHPE image')
        secondary = _PE(view).graph()
        if secondary['machine'] != 0x8664 or secondary['optionalMagic'] != 0x20b:
            raise ValueError('ARM64X fixups did not produce an AMD64/EC image view')
        if not secondary['chpe']['present']:
            raise ValueError('Derived EC image lacks CHPE metadata')
        architecture = 'arm64x'
        graphs = {'native': primary, 'ec': secondary}
    return {'schema': 1, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'machine': original.machine, 'machineHex': hex(original.machine),
            'optionalMagic': original.magic, 'architecture': architecture,
            'graphs': graphs, 'dynamicRelocations': dynamic,
            'runtimeVerified': False, 'scope': 'Static file/derived EC view; no loader, thunk or GPU execution'}


def _dynamic_cxx(module):
    lower = module.lower()
    return bool(re.match(r'(?:(?:lib)?(?:c\+\+|c\+\+abi|unwind|stdc\+\+|gcc_s)(?:[-_.0-9].*)?|msvcp[0-9].*|vcruntime[0-9].*)\.dll$', lower))


def validate_dxvk(data, module_name, architecture='arm64x'):
    """Validate the exact two-DLL ARM64X profile or an explicit PE32 counterpart."""
    module_name = module_name.lower()
    if module_name not in EXPECTED_EXPORTS:
        raise ValueError('No DXVK native profile for '+module_name)
    result = inspect_pe(data)
    if result['architecture'] != architecture:
        raise ValueError('DXVK architecture mismatch: '+result['architecture'])
    required_graphs = ('native', 'ec') if architecture == 'arm64x' else ('primary',)
    if architecture not in ('arm64x', 'i386', 'x86_64') or set(result['graphs']) != set(required_graphs):
        raise ValueError('Unqualified DXVK image graph layout')
    for name in required_graphs:
        graph = result['graphs'][name]
        if not graph['dll']:
            raise ValueError('DXVK image is not a DLL')
        if not isinstance(graph['exportDllName'], str) or graph['exportDllName'].lower() != module_name:
            raise ValueError('DXVK export DLL identity differs from installed basename')
        if any('.partial.' in dependency for dependency in graph['importModules']):
            raise ValueError('Temporary build filename leaked into DLL imports')
        if module_name == 'd3d11.dll' and 'dxgi.dll' not in graph['importModules']:
            raise ValueError('D3D11 does not import the installed dxgi.dll group member')
        if architecture == 'arm64x':
            types = {r['type'] for r in graph['chpe']['codeRanges']}
            if not {'arm64', 'arm64ec'} <= types:
                raise ValueError('CHPE does not describe both ARM64 code graphs')
        exports = {r['name']: r for r in graph['exports'] if r['name'] is not None}
        for symbol, ordinal in EXPECTED_EXPORTS[module_name].items():
            entry = exports.get(symbol)
            if not entry or entry['ordinal'] != ordinal or 'forwarder' in entry:
                raise ValueError('Wrong/missing DXVK name+ordinal in '+name+': '+symbol)
            if architecture == 'arm64x':
                kinds = [r['type'] for r in graph['chpe']['codeRanges']
                         if r['rva'] <= entry['rva'] < r['rva']+r['bytes']]
                allowed = {'arm64'} if name == 'native' else {'arm64ec', 'amd64'}
                if len(kinds) != 1 or kinds[0] not in allowed:
                    raise ValueError('Export targets wrong ARM64X code graph: '+symbol)
        forbidden = [m for m in graph['importModules'] if _dynamic_cxx(m)]
        if forbidden:
            raise ValueError('Dynamic C++ runtime imported by '+name+': '+','.join(forbidden))
    result['module'] = module_name
    result['validatedExportOrdinals'] = EXPECTED_EXPORTS[module_name]
    result['dynamicCppImportsAbsent'] = True
    return result


def runtime_loaded_match(record, inventory, expected_view=None):
    """Match bounded fixture headers to an actually derived image view.

    This does NOT bind the Windows path or hash to its file: the collector must
    separately resolve/hash the loaded module and bind inventory.sha256. A PE
    header match is a necessary view check, never a loader/thunk execution proof.
    """
    if not isinstance(record, dict) or not isinstance(inventory, dict) or inventory.get('schema') != 1:
        raise ValueError('Invalid loaded module/inventory record')
    observed = record.get('peView')
    if not isinstance(observed, dict):
        raise ValueError('Complete loaded PE view evidence required')
    fields = ('machine', 'optionalMagic', 'timeDateStamp', 'sizeOfImage',
              'loadConfigRva', 'loadConfigSize', 'chpeMetadataRva', 'chpeMetadataVersion',
              'exportDirectoryRva', 'exportDirectorySize', 'importDirectoryRva', 'importDirectorySize')
    if set(observed) != set(fields) or any(type(observed[k]) is not int or not 0 <= observed[k] <= 0xffffffff for k in fields):
        raise ValueError('Malformed loaded PE view fields')
    for name in ('machine', 'optionalMagic', 'timeDateStamp', 'sizeOfImage'):
        if name in record and (type(record[name]) is not int or record[name] != observed[name]):
            raise ValueError('Outer/loaded PE view identity mismatch')
    matches = [name for name, graph in inventory.get('graphs', {}).items()
               if isinstance(graph, dict) and graph.get('loadedHeader') == observed]
    if len(matches) != 1 or expected_view is not None and matches[0] != expected_view:
        raise ValueError('Loaded PE header does not uniquely match expected derived view')
    return matches[0]
