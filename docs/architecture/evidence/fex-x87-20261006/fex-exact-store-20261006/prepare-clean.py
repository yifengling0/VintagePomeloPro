"""Stage the same exact-store emitter without any float diagnostic code."""
from pathlib import Path
import difflib
import json
import shutil
import subprocess

repo = Path('/home/liufeng/src/vpp-proton')
previous = repo / 'workspace_temp/fex-exact-store-20261006'
out = repo / 'workspace_temp/fex-exact-clean-20261006'
base = repo / 'workspace_temp/fex-softfloat-lto-20261006/source'
source = out / 'source'
assert not (repo / 'build-fex-exact-clean-20261006').exists(), 'Do not restage an existing build'
out.mkdir(exist_ok=True)
if not source.exists():
    subprocess.run(['cp', '-a', '--reflink=auto', str(base), str(source)], check=True)
names = ['FEXCore/Source/Interface/Context/Context.h',
         'FEXCore/Source/Interface/Core/Core.cpp',
         'FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp']
header = source / names[0]
text = (base / names[0]).read_text()
anchor = '  bool WindowsJITDiagnosticsEnabled {};'
assert text.count(anchor) == 1
header.write_text(text.replace(anchor, anchor + '\n  bool WindowsExactFloatStoreEnabled {};'))
core = source / names[1]
text = (base / names[1]).read_text()
anchor = '  const char* JITPerf = std::getenv("FEX_JITPERF");'
assert text.count(anchor) == 1
core.write_text(text.replace(anchor,
    '  const char* ExactStore = std::getenv("FEX_EXACTSTORE");\n'
    '  WindowsExactFloatStoreEnabled = ExactStore && std::strcmp(ExactStore, "1") == 0;\n' + anchor))
# Dispatcher has no float diagnostic changes; copy the already tested emitter.
shutil.copyfile(previous / 'source' / names[2], source / names[2])
patch = ''.join(''.join(difflib.unified_diff((base / name).read_text().splitlines(True),
    (source / name).read_text().splitlines(True), fromfile='a/' + name, tofile='b/' + name)) for name in names)
(out / 'fex-exact-store.patch').write_text(patch)
assert not (source / 'FEXCore/include/FEXCore/Utils/FloatDiagnostics.h').exists()
for name in ['automate.py', 'capture.py', 'navigate.py', 'run.py', 'summarize.py',
             'exact-store-probe.c', 'exact-store-probe.exe']:
    local = Path('/mnt/f/VintagePomelo-Workspace/workspace_temp/fex-exact-store-20261006') / name
    shutil.copyfile(local if local.exists() else previous / name, out / name)
for name in ['package.py', 'sign-candidate.py', 'sign-with-local-material.py',
             'validate-source.py', 'validate-candidate.py', 'build-float-diag.sh']:
    text = (previous / name).read_text()
    text = text.replace('fex-exact-store-20261006', 'fex-exact-clean-20261006')
    text = text.replace('entry-default-fex-exact-store-', 'entry-default-fex-exact-clean-')
    text = text.replace('vp-fex-exact-store-sign-material-', 'vp-fex-exact-clean-sign-material-')
    if name == 'package.py':
        text = text.replace("base_root = repo / 'workspace_temp/main-proton-isolation-20261006'",
                            "base_root = repo / 'workspace_temp/fex-exact-store-20261006'")
        text = text.replace('92c703efe54b8cb74a9184f7ba3e42911113fa004b12798c0381b8f3eb9b2756',
                            '41faf76f605ca9969557ebf8974ec11bb61c313f9d1c4db8e982e3723aa40550')
        text = text.replace("assert b'[FEX-FLOAT-PERF]' in new_fex and b'FEX_FLOATPERF' in new_fex",
                            "assert b'[FEX-FLOAT-PERF]' not in new_fex and b'FEX_FLOATPERF' not in new_fex")
        text = text.replace("floatDiagnosticsPatchSha256=sha(out / 'fex-float-diagnostics.patch'), ", '')
        text = text.replace('float and JIT diagnostics remain disabled by default',
                            'float diagnostic code absent; inherited JIT diagnostics disabled by default')
        text = text.replace('Opt-in exact-normal x87 store fast path over float diagnostics base',
                            'Opt-in exact-normal x87 store with float diagnostic code removed')
    elif name == 'validate-source.py':
        text = text.replace('main-proton-isolation-20261006/source', 'fex-softfloat-lto-20261006/source')
    elif name == 'validate-candidate.py':
        start = text.index("with (out / 'fex-float-diagnostics.patch')")
        end = text.index("with (out / 'fex-exact-store.patch')", start)
        text = text[:start] + text[end:]
        text = text.replace("base_out = repo / 'workspace_temp/main-proton-isolation-20261006'",
                            "base_out = repo / 'workspace_temp/fex-exact-store-20261006'")
    (out / ('build.sh' if name == 'build-float-diag.sh' else name)).write_text(text, newline='\n')
# Only the comparison experiment had float diagnostics. Production-staged FEX
# receives a patch with no dependency on FloatDiagnostics or its hot-path branch.
(out / 'source-basis.json').write_text(json.dumps({
    'base': str(base), 'candidate': str(source), 'changedFiles': names,
    'floatDiagnosticCodeIncluded': False, 'defaultEnabled': False}, indent=2) + '\n')
print(out)
