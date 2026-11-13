"""Verify real CRT open methods against a Windows API capture seam."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/fex-windows-crt-file-open.patch'
PATH = 'Source/Windows/Common/CRT/IO.cpp'


def production():
    return subprocess.check_output(['git', '-C', str(ROOT / 'thirdparty/fex'),
        'show', '86ff33bbe2:' + PATH], text=True)


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


STUBS = r'''
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#define DLLEXPORT_FUNC(type, name, params) type name params
#define _O_RDONLY 0
#define _O_WRONLY 1
#define _O_RDWR 2
#define _O_CREAT 0x100
#define _O_TRUNC 0x200
#define _O_EXCL 0x400
#define _O_APPEND 8
#define _S_IREAD 0x100
#define _S_IWRITE 0x80
#define _SH_DENYRW 0x10
#define _SH_DENYWR 0x20
#define _SH_DENYRD 0x30
#define _SH_DENYNO 0x40
using DWORD = unsigned;
using ULONG = unsigned;
using HANDLE = long;
static constexpr HANDLE INVALID_HANDLE_VALUE = -1;
static constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
static constexpr DWORD FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
static constexpr DWORD FILE_ATTRIBUTE_READONLY = 1;
static constexpr DWORD CREATE_ALWAYS = 2, CREATE_NEW = 1, TRUNCATE_EXISTING = 5;
static constexpr DWORD OPEN_ALWAYS = 4, OPEN_EXISTING = 3;
static constexpr DWORD ERROR_FILE_EXISTS = 80, ERROR_FILE_NOT_FOUND = 2, ERROR_ACCESS_DENIED = 5;
struct UNICODE_STRING { wchar_t* Buffer; };
static bool fail_conversion;
static int conversions, frees;
static bool RtlCreateUnicodeStringFromAsciiz(UNICODE_STRING* output, const char* input) {
  ++conversions;
  if (fail_conversion) return false;
  size_t size = strlen(input) + 1;
  output->Buffer = static_cast<wchar_t*>(malloc(size * sizeof(wchar_t)));
  for (size_t i = 0; i < size; ++i) output->Buffer[i] = static_cast<unsigned char>(input[i]);
  return true;
}
static void RtlFreeUnicodeString(UNICODE_STRING* string) { ++frees; free(string->Buffer); }
static DWORD capture_access, capture_sharing, capture_creation, capture_attributes;
static int api_calls;
static std::wstring capture_filename;
static HANDLE CreateFileW(const wchar_t* name, DWORD access, DWORD sharing, void*, DWORD creation, DWORD attrs, void*) {
  ++api_calls; capture_filename = name; capture_access = access;
  capture_sharing = sharing; capture_creation = creation; capture_attributes = attrs;
  return 7;
}
static DWORD GetLastError() { return 0; }
namespace fixture {
struct FILE { FILE(HANDLE, int, bool) {} };
static int AllocateFile(std::unique_ptr<FILE>&&) { return 11; }
'''

MAIN = r'''
}
int main() {
  using namespace fixture;
  assert(_sopen("C:\\profile.json", _O_RDONLY, _SH_DENYNO) == 11);
#ifdef EXPECT_OLD
  assert(capture_access == 0); // _O_RDONLY is zero, so old code never requests read access.
#else
  assert(capture_access == GENERIC_READ);
#endif
  assert(capture_filename == L"C:\\profile.json" && capture_creation == OPEN_EXISTING);
  assert(_sopen("C:\\perf.map", _O_CREAT | _O_RDWR, _SH_DENYRD, _S_IREAD | _S_IWRITE) == 11);
#ifdef EXPECT_OLD
  assert(capture_attributes == FILE_ATTRIBUTE_READONLY);
  assert(capture_sharing == FILE_SHARE_READ);
#else
  assert(capture_access == (GENERIC_READ | GENERIC_WRITE));
  assert(capture_attributes == 0);
  assert(capture_sharing == FILE_SHARE_WRITE);
#endif
  assert(capture_creation == OPEN_ALWAYS);
#ifndef EXPECT_OLD
  assert(_sopen("C:\\readonly.txt", _O_CREAT | _O_WRONLY, _SH_DENYRW, _S_IREAD) == 11);
  assert(capture_access == GENERIC_WRITE && capture_sharing == 0);
  assert(capture_attributes == FILE_ATTRIBUTE_READONLY);
  assert(_sopen("C:\\shared.txt", _O_RDWR, _SH_DENYWR) == 11);
  assert(capture_access == (GENERIC_READ | GENERIC_WRITE) && capture_sharing == FILE_SHARE_READ);
  assert(_sopen("C:\\exclusive.txt", _O_CREAT | _O_EXCL | _O_WRONLY, _SH_DENYNO, _S_IWRITE) == 11);
  assert(capture_creation == CREATE_NEW && capture_attributes == 0);
  assert(_sopen("C:\\truncate.txt", _O_CREAT | _O_TRUNC | _O_WRONLY, _SH_DENYNO, _S_IWRITE) == 11);
  assert(capture_creation == CREATE_ALWAYS && capture_attributes == 0);
#endif
  assert(conversions == frees);
  int previous_calls = api_calls;
  fail_conversion = true;
  assert(_sopen("bad", _O_RDONLY, _SH_DENYNO) == -1);
  assert(errno == EINVAL && api_calls == previous_calls && conversions == frees + 1);
}
'''


class FileOpen(unittest.TestCase):
    def test_production_forwarding_and_original_regression(self):
        old = production()
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            path = work / PATH
            path.parent.mkdir(parents=True)
            path.write_text(old)
            subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            changed = path.read_text()
            subprocess.run(['patch', '-p1', '-R', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            self.assertEqual(path.read_text(), old)
            subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            self.assertEqual(path.read_text(), changed)
            for label, source, flags in [('old', old, ['-DEXPECT_OLD']), ('candidate', changed, [])]:
                methods = '\n'.join(function(source, signature) for signature in (
                    'int ErrnoReturn(', 'DWORD OpenFlagToAccess(', 'DWORD OpenFlagToCreation(',
                    'DLLEXPORT_FUNC(int, _wsopen,', 'DLLEXPORT_FUNC(int, _wopen,', 'DLLEXPORT_FUNC(int, _sopen,'))
                cpp, executable = work / (label + '.cpp'), work / label
                cpp.write_text(STUBS + methods + MAIN)
                subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', *flags,
                    str(cpp), '-o', str(executable)], check=True, capture_output=True)
                subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
