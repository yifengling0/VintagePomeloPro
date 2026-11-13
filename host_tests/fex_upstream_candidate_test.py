"""Compile migrated FEX functions and exercise their real boundary behavior.

The host harness controls Windows APIs; it does not measure ARM JIT speed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host_tests'))
from fex_wow64_synthetic_return_test import STUBS, MAIN, function
from fex_jit_cost_test import CPP as DIAGNOSTICS

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
args, extra = parser.parse_known_args()
SOURCE = args.source.resolve()


def execute(code, cases=()):
    with tempfile.TemporaryDirectory(prefix='fex-upstream-regression-') as directory:
        path = Path(directory)
        (path / 'test.cpp').write_text(code)
        result = subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra',
                                 '-I' + str(SOURCE / 'FEXCore/include'),
                                 str(path / 'test.cpp'), '-o', str(path / 'test')],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr[-5000:])
        for case in cases or (None,):
            result = subprocess.run([str(path / 'test')] + ([case] if case else []),
                                    capture_output=True, text=True, timeout=15)
            if result.returncode:
                raise AssertionError(f'{case}: {result.returncode}: {result.stderr}')
            print(result.stdout, end='')


class TreeIdentityTests(unittest.TestCase):
    def fixture(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        from prepare_fex_upstream import verify_tree
        self.verify = verify_tree
        # Git is the independent oracle for file modes and tree ordering.
        with tempfile.TemporaryDirectory(prefix='fex-tree-oracle-') as directory:
            subprocess.run(['git', 'init', '-q', directory], check=True)
            entries = [{'path': 'source.cpp', 'mode': '100644', 'type': 'blob', 'sha': '1' * 40},
                       {'path': 'run', 'mode': '100755', 'type': 'blob', 'sha': '2' * 40},
                       {'path': 'link', 'mode': '120000', 'type': 'blob', 'sha': '3' * 40}]
            data = ''.join(f"{row['mode']} blob {row['sha']}\t{row['path']}\n" for row in entries).encode()
            expected = subprocess.check_output(['git', '-C', directory, 'mktree', '--missing'], input=data).decode().strip()
        return {'sha': '4' * 40, 'tree': entries, 'truncated': False}, expected

    def test_api_commit_query_alias_does_not_replace_real_tree(self):
        value, expected = self.fixture()
        self.assertNotEqual(value['sha'], expected)
        self.assertEqual(self.verify(value, expected), expected)

    def test_incomplete_or_modified_dependency_inventory_is_rejected(self):
        value, expected = self.fixture()
        value['tree'].pop()
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.verify(value, expected)
        value['truncated'] = True
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            self.verify(value, expected)

    def test_duplicate_paths_cannot_hide_changed_tree_entries(self):
        value, expected = self.fixture()
        value['tree'].append(value['tree'][0].copy())
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.verify(value, expected)


class MigratedBoundaryTests(unittest.TestCase):
    def test_locked_patch_series_recreates_complete_candidate_tree(self):
        lock = json.loads((ROOT / 'docs/upstream-sync/fex-candidate.lock.json').read_text())
        with tempfile.TemporaryDirectory(prefix='fex-upstream-index-') as directory:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(directory) / 'index'))
            def git(*args):
                return subprocess.check_output(['git', '-C', str(SOURCE), *args], env=env)
            git('read-tree', lock['upstream']['commit'])
            for item in lock['patches']:
                patch = ROOT / item['path']
                self.assertEqual(hashlib.sha256(patch.read_bytes()).hexdigest(), item['sha256'])
                git('apply', '--cached', '--whitespace=nowarn', str(patch))
            self.assertEqual(git('write-tree').decode().strip(), lock['adaptedTree'])

    def test_fresh_replay_restores_exact_adapted_commit_history(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        from prepare_fex_upstream import replay_history, verify_source
        lock = json.loads((ROOT / 'docs/upstream-sync/fex-candidate.lock.json').read_text())
        with tempfile.TemporaryDirectory(prefix='fex-history-replay-') as directory:
            # Only sparse changed files are checked out. All immutable objects
            # are reused from the candidate; no complete source copy is made.
            repo = Path(directory)
            subprocess.run(['git', 'init', '-q', directory], check=True)
            common = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse',
                '--path-format=absolute', '--git-common-dir']).decode().strip()
            (repo / '.git/objects/info/alternates').write_text(common + '/objects\n')
            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args])
            git('config', 'core.sparseCheckout', 'true')
            (repo / '.git/info/sparse-checkout').write_text('FEXCore/\nSource/Windows/\n')
            git('checkout', '-q', '-b', 'ohos/candidate', lock['upstream']['commit'])
            replay_history(repo, lock)
            self.assertEqual(git('rev-parse', 'HEAD').decode().strip(), lock['adaptedCommit'])
            self.assertEqual(git('write-tree').decode().strip(), lock['adaptedTree'])
            self.assertEqual(git('status', '--porcelain').strip(), b'')
            git('update-ref', 'refs/heads/ohos/candidate', lock['upstream']['commit'])
            with self.assertRaisesRegex(ValueError, 'HEAD differs'):
                verify_source(repo, lock)

    def test_synthetic_returns_with_upstream_context_protocol(self):
        module = (SOURCE / 'Source/Windows/WOW64/Module.cpp').read_text()
        header = (SOURCE / 'Source/Windows/Common/CallRetStack.h').read_text()
        helper = function(header, 'bool CompleteSyntheticReturn(')
        handler = function(module, 'static void HandleSyscallImpl(')
        stubs = STUBS.replace('UnlockJITContext(int)', 'UnlockJITContext()')
        stubs = stubs.replace('LockJITContext(int)', 'LockJITContext()')
        stubs = stubs.replace('namespace Context {', 'namespace Context {\nstatic void FlushThreadStateContext() {}')
        stubs = stubs.replace('  return 0x2468;', '''  if (mode == 5) { frame.State.gregs[1] = 0x5678; }
  return 0x2468;''')
        main = MAIN[MAIN.index('static void push'):]
        start = main.index('  if (!strcmp(argv[1], "old-leak"))')
        end = main.index('!strcmp(argv[1], "loop")', start)
        main = main[:start] + '  if (' + main[end:]
        main = main.replace('HandleSyscallImpl(&frame, nullptr)', 'HandleSyscallImpl(&frame)')
        main = main.replace('frame.State.gregs[0] == 9 && frame.State.gregs[1] == uintptr_t(guest)',
                            'frame.State.gregs[0] == 0x2468 && frame.State.gregs[1] == uintptr_t(guest) + 20')
        main = main.replace('  } else if (!strcmp(argv[1], "moved")) {', '''  } else if (!strcmp(argv[1], "stack-context")) {
    push(0x1122); entry(); mode = 5; auto sp = frame.State.callret_sp;
    HandleSyscallImpl(&frame);
    assert(frame.State.rip == 0x1122 && frame.State.gregs[1] == 0x5678);
    assert(frame.State.callret_sp == sp);
  } else if (!strcmp(argv[1], "moved")) {''')
        code = '#include <initializer_list>\n' + stubs
        code += '\nnamespace FEX::Windows::CallRetStack {\n' + helper + '\n}\n' + handler + '\n' + main
        execute(code, ('loop', 'context', 'stack-context', 'moved', 'callback',
                       'invalidate', 'mismatch', 'bounds', 'empty'))

    def test_unaligned_optional_result_is_not_a_boolean(self):
        module = (SOURCE / 'Source/Windows/WOW64/Module.cpp').read_text()
        handler = function(module, 'std::optional<int32_t> HandleUnalignedAccess(')
        code = r'''
#include <cassert>
#include <cstdint>
#include <optional>
struct CONTEXT { uint64_t Pc{}, X0{}; };
static bool InCode = true;
static std::optional<int32_t> Result;
static unsigned Calls;
struct ContextStub {
 bool IsAddressInCodeBuffer(void*,uint64_t) { return InCode; }
} Ctx;
struct ThreadStub { ContextStub* CTX = &Ctx; } Thread;
struct TLSStub { ThreadStub* ThreadState() { return &Thread; } };
static TLSStub GetTLS() { return {}; }
struct Config { int GetUnalignedHandlerType() { return 0; } } ConfigValue;
static Config* HandlerConfig = &ConfigValue;
namespace FEXCore::ArchHelpers::Arm64 {
static std::optional<int32_t> HandleUnalignedAccess(ThreadStub*,int,uint64_t,uint64_t*) {
 ++Calls; return Result;
}
}
'''
        code += handler + r'''
int main() {
 CONTEXT C{0x1000,0};
 InCode=false; Result=4; assert(!HandleUnalignedAccess(&C)); assert(!Calls && C.Pc==0x1000);
 InCode=true; Result=std::nullopt; assert(!HandleUnalignedAccess(&C)); assert(Calls==1 && C.Pc==0x1000);
 Result=0; auto Zero=HandleUnalignedAccess(&C); assert(Zero.has_value() && *Zero==0 && C.Pc==0x1000);
 Result=4; assert(HandleUnalignedAccess(&C)==4 && C.Pc==0x1004);
 Result=-4; assert(HandleUnalignedAccess(&C)==-4 && C.Pc==0x1000);
}
'''
        execute(code)

    def test_opt_in_diagnostic_accounting(self):
        execute(DIAGNOSTICS)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *extra], verbosity=2)
