"""Count instructions in the actual packaged PE32 DLLs; not runtime attribution."""
from collections import Counter
from pathlib import Path
import hashlib
import io
import json
import re
import subprocess
import tempfile
import zipfile

repo = Path('/data/src/winehua')
out = repo / 'workspace_temp/fex-exact-clean-20261006'
ident = json.loads((out / 'package-identity.json').read_text())
tool = repo / '.temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64/bin/llvm-objdump'
result = {}
with zipfile.ZipFile(out / ident['unsignedHap']['filename']) as hap:
    with zipfile.ZipFile(io.BytesIO(hap.read('resources/rawfile/wine-data.zip'))) as payload:
        for name in ['wined3d.dll', 'd3d9.dll', 'dsound.dll', 'winmm.dll']:
            key = 'bin/i386-windows/' + name
            if key not in payload.namelist():
                result[name] = {'missing': True}
                continue
            data = payload.read(key)
            with tempfile.TemporaryDirectory(prefix='vp-pe-math-') as folder:
                path = Path(folder) / name
                path.write_bytes(data)
                disasm = subprocess.run([str(tool), '-d', '--no-show-raw-insn', str(path)],
                    capture_output=True, text=True, check=True).stdout
            ops = Counter()
            x87_functions = Counter()
            function = '(unknown)'
            for line in disasm.splitlines():
                label = re.match(r'^[0-9a-f]+ <(.+)>:', line)
                if label:
                    function = label[1]
                instruction = re.match(r'^\s*[0-9a-f]+:\s+(\w+)(?:\s|$)', line)
                if instruction:
                    op = instruction[1]
                    ops[op] += 1
                    if op.startswith('f'):
                        x87_functions[function] += 1
            result[name] = {'sha256': hashlib.sha256(data).hexdigest(),
                'x87StaticInstructions': {k: v for k, v in ops.items() if k.startswith('f')},
                'nearestLabelsByStaticX87Count': x87_functions.most_common(12),
                'limitation': 'Static opcode counts, not runtime cost. Stripped PE labels can span multiple actual functions; labels are not exact function attribution.'}
(out / 'packaged-pe-math.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
