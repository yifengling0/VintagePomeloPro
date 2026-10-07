"""Replay ARM64 leaf unwind and its real caller from pinned Wine source.

Metadata/logging are host test seams; the old and new unwind functions and PC
adjustment come from production source. This does not emulate FEX JIT execution.
"""
from pathlib import Path
import os, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = 'cd547f7a0e'

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

PRELUDE = '\n#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\ntypedef unsigned long ULONG, ULONG_PTR, DWORD64;\ntypedef int BOOL, BOOLEAN, NTSTATUS;\ntypedef struct { unsigned Flag; } ARM64_RUNTIME_FUNCTION;\ntypedef struct { uint64_t D[2]; } VECTOR;\ntypedef struct { uint64_t Pc,Lr,Sp; unsigned ContextFlags; uint64_t X19,rest[10]; VECTOR V[32]; } ARM64_NT_CONTEXT;\ntypedef ARM64_NT_CONTEXT CONTEXT;\ntypedef void *PEXCEPTION_ROUTINE;\ntypedef void KNONVOLATILE_CONTEXT_POINTERS_ARM64;\ntypedef struct { uint64_t GpNvRegs[11],FpNvRegs[8]; } DISPATCHER_CONTEXT_NONVOLREG_ARM64;\ntypedef struct { unsigned ScopeIndex; uint64_t ControlPc,ImageBase,EstablisherFrame; BOOL ControlPcIsUnwound; void *HistoryTable,*NonVolatileRegisters,*HandlerData; ARM64_RUNTIME_FUNCTION *FunctionEntry; PEXCEPTION_ROUTINE LanguageHandler; } DISPATCHER_CONTEXT;\ntypedef struct { struct { void *Buffer; } BaseDllName; void *DllBase; } LDR_DATA_TABLE_ENTRY;\n#define WINAPI\n#define TRUE 1\n#define FALSE 0\n#define STATUS_SUCCESS 0\n#define STATUS_BAD_FUNCTION_TABLE ((int)0xc00000ff)\n#define STATUS_ACCESS_VIOLATION ((int)0xc0000005)\n#define STATUS_INVALID_DISPOSITION ((int)0xc0000026)\n#define CONTEXT_UNWOUND_TO_CALL 0x20000000u\n#define TRACE(...) ((void)0)\n#define WARN(...) ((void)0)\n#define FIXME(...) ((void)0)\n#define WINE_BACKTRACE_LOG(...) ((void)0)\n#define __TRY if (1)\n#define __EXCEPT_PAGE_FAULT else if (0)\n#define __ENDTRY\nstatic void *unwind_packed_data(ULONG_PTR b,ULONG_PTR p,ARM64_RUNTIME_FUNCTION*f,ARM64_NT_CONTEXT*c,void*q) { c->Sp += 16; return 0; }\nstatic void *unwind_full_data(ULONG_PTR b,ULONG_PTR p,ARM64_RUNTIME_FUNCTION*f,ARM64_NT_CONTEXT*c,void*d,void*q,BOOLEAN*r) { c->Sp += 16; return 0; }\nstatic ARM64_RUNTIME_FUNCTION *RtlLookupFunctionEntry(ULONG_PTR p,ULONG_PTR*b,void*t) { *b=0;return 0; }\nstatic int LdrFindEntryForAddress(void *p,LDR_DATA_TABLE_ENTRY**m) { return 1; }\n'
MAIN = '\nint main(void) {\n  const uint64_t lr=0x7ff5996cc0, sp=0x1001ff2e0;\n  CONTEXT c={.Pc=0,.Lr=lr,.Sp=sp};\n  DISPATCHER_CONTEXT_NONVOLREG_ARM64 nv={0};\n  DISPATCHER_CONTEXT d={.NonVolatileRegisters=&nv};\n  assert(old_virtual_unwind(1,&d,&c,0)==STATUS_SUCCESS);\n  for(int i=0;i<10000;i++) {\n    assert(old_virtual_unwind(1,&d,&c,0)==STATUS_SUCCESS);\n    assert(c.Pc==lr && c.Lr==lr && c.Sp==sp);\n  }\n  puts("old production caller: 10000 successful unwinds without progress");\n  c=(CONTEXT){.Pc=0,.Lr=lr,.Sp=sp};\n  assert(virtual_unwind(1,&d,&c,0)==STATUS_SUCCESS);\n  assert(c.Pc==lr && c.Sp==sp);\n  assert(virtual_unwind(1,&d,&c,0)==STATUS_INVALID_DISPOSITION);\n  puts("patched production caller: second metadata-free unwind stops");\n  void *data=0; ULONG_PTR frame=0;\n  c=(CONTEXT){.Pc=lr,.Lr=lr,.Sp=sp};\n  assert(RtlVirtualUnwind2(1,0,lr,0,&c,0,&data,&frame,0,0,0,0,0)==STATUS_BAD_FUNCTION_TABLE);\n  c=(CONTEXT){.Pc=lr,.Lr=lr,.Sp=sp,.ContextFlags=CONTEXT_UNWOUND_TO_CALL};\n  assert(RtlVirtualUnwind2(1,0,lr-4,0,&c,0,&data,&frame,0,0,0,0,0)==STATUS_BAD_FUNCTION_TABLE);\n  ARM64_RUNTIME_FUNCTION func={.Flag=1};\n  assert(RtlVirtualUnwind2(1,0,lr-4,&func,&c,0,&data,&frame,0,0,0,0,0)==STATUS_SUCCESS);\n  assert(c.Sp==sp+16);\n  puts("direct and adjusted invalid leaf rejected; metadata path still runs");\n}\n'

class Arm64LeafUnwind(unittest.TestCase):
    def test_production_progress(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            paths = ['dlls/ntdll/unwind.c', 'dlls/ntdll/signal_arm64.c']
            for name in paths:
                p = tree / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(subprocess.check_output(['git', '-C', str(ROOT/'thirdparty/wine-valve'),
                    'show', f'{PIN}:{name}'], text=True))
            old_source = (tree/paths[0]).read_text()
            signature = 'NTSTATUS WINAPI RtlVirtualUnwind2( ULONG type, ULONG_PTR base, ULONG_PTR pc,\n                                   ARM64_RUNTIME_FUNCTION'
            old = function(old_source, signature)
            with (ROOT/'patches/wine/0045-ntdll-arm64-invalid-leaf-unwind.patch').open() as patch:
                subprocess.run(['patch','--batch','--force','-s','-p1'],cwd=tree,stdin=patch,check=True)
            new_source = (tree/paths[0]).read_text()
            new = function(new_source, signature)
            self.assertEqual(new_source.replace(new, old, 1), old_source,
                             'Only the ARM64 function may change, not ARM32')
            caller = function((tree/paths[1]).read_text(), 'static NTSTATUS virtual_unwind(')
            source = PRELUDE + old.replace('RtlVirtualUnwind2(', 'old_RtlVirtualUnwind2(', 1) + '\n'
            source += caller.replace('virtual_unwind(', 'old_virtual_unwind(', 1).replace('RtlVirtualUnwind2(', 'old_RtlVirtualUnwind2(')
            source += '\n' + new + '\n' + caller + '\n' + MAIN
            cpp = tree/'replay.c'
            cpp.write_text(source)
            subprocess.run([os.environ.get('CC','cc'),'-O2',str(cpp),'-o',str(tree/'replay')],check=True)
            result = subprocess.run([str(tree/'replay')],capture_output=True,text=True,check=True)
            print(result.stdout,end='')

if __name__ == '__main__':
    unittest.main()
