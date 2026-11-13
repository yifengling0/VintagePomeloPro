#!/usr/bin/env python3
"""Execute FEX's production fault-boundary classifier before and after the fix.

This covers metadata selection; PE probes separately exercise actual ARM JIT
replay, including POP register allocation and stack-based effective addresses.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'thirdparty/fex/FEXCore/Source/Interface/Core/OpcodeDispatcher.h'
PATCH = ROOT / 'scripts/patches/fex-memory-offset-fault-boundary.patch'

def function(source):
    start = source.index('  static bool CanHaveSideEffects(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

STUBS = r'''
#include <cstdio>
namespace FEXCore { namespace X86Tables {
namespace InstFlags { enum { FLAGS_DEBUG_MEM_ACCESS=1, FLAGS_MEM_OFFSET=2,
                            FLAGS_SETS_RIP=4, FLAGS_BLOCK_END=8 }; }
struct X86InstInfo { unsigned Flags; };
struct DecodedOperand {
  enum Kind { None, Literal, Register, Direct, Indirect, RIPRelative, SIB } kind;
  bool IsNone() const { return kind == None; }
  bool IsGPRDirect() const { return kind == Direct; }
  bool IsGPRIndirect() const { return kind == Indirect; }
  bool IsRIPRelative() const { return kind == RIPRelative; }
  bool IsSIB() const { return kind == SIB; }
};
struct DecodedData { DecodedOperand Dest, Src[3]; };
using DecodedOp = const DecodedData*;
} }
namespace X86Tables = FEXCore::X86Tables;
struct Production {
'''
MAIN = r'''
};
int main() {
  using namespace X86Tables;
  unsigned failures=0, checks=0;
  auto check=[&](bool actual,bool expected) { ++checks; failures += actual != expected; };
  DecodedData op{};
  X86InstInfo table{};
  // A0..A3 decode moffs as a literal; normal immediate MOV is not a memory op.
  op.Dest.kind=DecodedOperand::Literal;
  check(Production::CanHaveSideEffects(&table,&op),false);
  table.Flags=InstFlags::FLAGS_MEM_OFFSET;
  check(Production::CanHaveSideEffects(&table,&op),true);
  op.Dest.kind=DecodedOperand::Register;
  op.Src[0].kind=DecodedOperand::Literal;
  check(Production::CanHaveSideEffects(&table,&op),true);
  table.Flags=0;
  check(Production::CanHaveSideEffects(&table,&op),false);
  for(auto kind : {DecodedOperand::Direct,DecodedOperand::Indirect,DecodedOperand::RIPRelative,DecodedOperand::SIB}) {
    op.Dest.kind=kind;
    check(Production::CanHaveSideEffects(&table,&op),true);
    check(Production::CanHaveSideEffects(nullptr,&op),true);
  }
  op={};
  for(unsigned flags : {InstFlags::FLAGS_DEBUG_MEM_ACCESS,InstFlags::FLAGS_SETS_RIP,InstFlags::FLAGS_BLOCK_END}) {
    table.Flags=flags;
    check(Production::CanHaveSideEffects(&table,&op),true);
  }
  std::printf("classifier checks=%u failures=%u\n",checks,failures);
  return failures;
}
'''

with tempfile.TemporaryDirectory(prefix='vp-fex-boundary-') as tmp:
    tree = Path(tmp)
    rel = HEADER.relative_to(ROOT / 'thirdparty/fex')
    target = tree / rel
    target.parent.mkdir(parents=True)
    target.write_bytes(HEADER.read_bytes())
    patch = PATCH.read_bytes()
    reverse = subprocess.run(['patch','-p1','--fuzz=0','-R','--dry-run'],cwd=tree,input=patch,
                             stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if reverse.returncode == 0:
        subprocess.run(['patch','-p1','--fuzz=0','-R'],cwd=tree,input=patch,check=True,stdout=subprocess.PIPE)
    before = function(target.read_text())
    subprocess.run(['patch','-p1','--fuzz=0'],cwd=tree,input=patch,check=True,stdout=subprocess.PIPE)
    after = function(target.read_text())
    for name, body, expected_failures in [('old',before,2),('fixed',after,0)]:
        c = tree / (name+'.cpp')
        c.write_text('#include <initializer_list>\n'+STUBS+body+MAIN)
        exe = tree / name
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
        result = subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode == expected_failures, result.stdout + result.stderr
        print(name+': '+result.stdout.strip())
print('PASS: moffs reads/stores gain fault boundaries; immediate/register operations and existing boundaries retain behavior')
