"""Exercise the production Windows reader and early-exception gate.

WinAPI capture seams validate native-read semantics, not ARM JIT execution.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/fex-windows-file-loading.patch'
PATHS = ['FEXCore/Source/Utils/FileLoading.cpp', 'Source/Common/Config.cpp', 'Source/Windows/WOW64/Module.cpp', 'FEXCore/Source/Interface/Config/Config.cpp']

STUBS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>
namespace fextl { using string = std::string; }
using DWORD = uint32_t;
using HANDLE = long;
using ssize_t = long;
constexpr HANDLE INVALID_HANDLE_VALUE = -1;
constexpr DWORD GENERIC_READ=0x80000000, FILE_SHARE_READ=1, FILE_SHARE_WRITE=2, FILE_SHARE_DELETE=4;
constexpr DWORD OPEN_EXISTING=3, FILE_ATTRIBUTE_NORMAL=0x80, FILE_END=2, FILE_BEGIN=0, ERROR_HANDLE_EOF=38;
struct LARGE_INTEGER { int64_t QuadPart{}; };
static std::string content = "{\"Config\":{}}\r\n";
static bool exists=true, seek_fail=false, read_fail=false, directory=false, info_fail=false;
static unsigned closes, reads, opens;
static size_t pos, max_read=3;
static DWORD error, last_request;
static int fail_after=-1;
static HANDLE CreateFileA(const char*, DWORD access, DWORD share, void*, DWORD creation, DWORD, void*) {
  ++opens; pos=0;
  assert(access==GENERIC_READ && share==7 && creation==OPEN_EXISTING);
  return exists ? 7 : INVALID_HANDLE_VALUE;
}
static bool CloseHandle(HANDLE handle) { assert(handle==7); ++closes; return true; }
struct FILE_STANDARD_INFO { bool Directory{}; };
constexpr int FileStandardInfo=1;
static bool GetFileInformationByHandleEx(HANDLE handle, int kind, FILE_STANDARD_INFO* info, size_t size) {
  assert(handle==7 && kind==FileStandardInfo && size==sizeof(*info));
  info->Directory=directory;
  return !info_fail;
}
static bool SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER* end, DWORD method) {
  if (seek_fail) return false;
  pos=method==FILE_END ? content.size() : 0;
  if (end) end->QuadPart=pos;
  return true;
}
static bool ReadFile(HANDLE, void* data, DWORD count, DWORD* read, void*) {
  last_request=count; ++reads;
  if (read_fail || (fail_after>=0 && reads>static_cast<unsigned>(fail_after))) { *read=99; error=5; return false; }
  *read=std::min({size_t(count), max_read, content.size()-pos});
  if (!*read) { error=ERROR_HANDLE_EOF; return false; }
  memcpy(data, content.data()+pos, *read); pos+=*read; return true;
}
static DWORD GetLastError() { return error; }
'''

MAIN = r'''
int main() {
  std::vector<char> data;
  assert(LoadFileImpl(data,"config",0) && std::string(data.begin(),data.end())==content);
  assert(reads>1 && closes==1);
  directory=true; const auto before_directory_reads=reads, before_directory_closes=closes;
  assert(!LoadFileImpl(data,"AppConfig/",0) && reads==before_directory_reads && closes==before_directory_closes+1);
  assert(LoadFileToBuffer("AppConfig/",std::span<char>{})==-1 && closes==before_directory_closes+2);
  directory=false; info_fail=true;
  assert(!LoadFileImpl(data,"config",0) && closes==before_directory_closes+3);
  info_fail=false; const auto before_empty_opens=opens;
  assert(!LoadFileImpl(data,"",0) && opens==before_empty_opens);
  assert(LoadFileToBuffer("",std::span<char>{})==-1 && opens==before_empty_opens);
  assert(LoadFileImpl(data,"config",3) && std::string(data.begin(),data.end())==content.substr(0,3));
  assert(data.size()==3);
  char buffer[4]{};
  assert(LoadFileToBuffer("config",buffer)==3 && std::string(buffer,3)==content.substr(0,3));
  exists=false; const auto closed=closes;
  assert(!LoadFileImpl(data,"missing",0) && closes==closed);
  assert(LoadFileToBuffer("missing",buffer)==-1 && closes==closed);
  exists=true; seek_fail=true;
  assert(!LoadFileImpl(data,"config",0) && closes==closed+1);
  seek_fail=false; read_fail=true;
  assert(!LoadFileImpl(data,"config",0) && data.empty());
  assert(LoadFileToBuffer("config",buffer)==-1);
  read_fail=false; reads=0; fail_after=1;
  assert(!LoadFileImpl(data,"config",0) && data.empty());
  fail_after=-1; content.clear();
  assert(LoadFileImpl(data,"empty",0) && data.empty());
  assert(LoadFileToBuffer("empty",buffer)==0);
  assert(!LoadFileImpl(data,"empty",1) && data.empty());
  assert(LoadFileToBuffer("empty",std::span<char>{})==0);
  content="{\"Config\":{}}\r\n";
  assert(!LoadFileImpl(data,"short",content.size()+1) && data.empty());
  {
    WindowsReadFile file("config"); max_read=1;
    assert(file.Read(buffer,size_t(UINT32_MAX)+10)==1 && last_request==UINT32_MAX);
  }
}
'''


class FileLoading(unittest.TestCase):
    def test_native_reader_and_early_gate(self):
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            originals = {}
            for name in PATHS:
                text = subprocess.check_output(['git', '-C', str(ROOT / 'thirdparty/fex'), 'show', '86ff33bbe2:' + name], text=True)
                originals[name] = text
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            for _ in range(2):
                subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
                changed = {name: (work / name).read_text() for name in PATHS}
                subprocess.run(['patch', '-p1', '-R', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
                self.assertEqual(originals, {name: (work / name).read_text() for name in PATHS})
            source = changed[PATHS[0]]
            native = source[source.index('class WindowsReadFile'):source.index('\n#endif', source.index('class WindowsReadFile'))]
            cpp = work / 'reader.cpp'
            cpp.write_text(STUBS + native + MAIN)
            subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(work / 'reader')], check=True, capture_output=True)
            subprocess.run([str(work / 'reader')], check=True)
            module = changed[PATHS[2]]
            start = module.index('bool BTCpuResetToConsistentStateImpl(')
            end = module.index('  FEXCORE_PROFILE_ACCUMULATION(Thread, AccumulatedSignalTime);', start)
            gate = module[start:end] + '  return true;\n}\n'
            gate_source = r'''
#include <atomic>
#include <cassert>
struct EXCEPTION_POINTERS { void *ContextRecord{}, *ExceptionRecord{}; };
static std::atomic<bool> CPUProcessReady{};
static int tls_reads;
static void *thread_state=reinterpret_cast<void*>(2);
struct TLS { void* ThreadState() { ++tls_reads; return thread_state; } };
static TLS GetTLS() { return {}; }
'''
            gate_source += gate + r'''
int main() {
  EXCEPTION_POINTERS pointers;
  assert(!BTCpuResetToConsistentStateImpl(&pointers) && tls_reads==0);
  CPUProcessReady=true; thread_state=nullptr;
  assert(!BTCpuResetToConsistentStateImpl(&pointers) && tls_reads==1);
  thread_state=reinterpret_cast<void*>(0x10000);
  assert(BTCpuResetToConsistentStateImpl(&pointers) && tls_reads==2);
}
'''
            gate_cpp = work / 'gate.cpp'
            gate_cpp.write_text(gate_source)
            subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-variable', str(gate_cpp), '-o', str(work / 'gate')], check=True, capture_output=True)
            subprocess.run([str(work / 'gate')], check=True)
            paths_source = changed[PATHS[3]]
            paths_start = paths_source.index('fextl::string GetApplicationConfig(')
            paths_end = paths_source.index('\nstatic fextl::map', paths_start)
            paths_fixture = r'''
#include <cassert>
#include <string>
#include <string_view>
namespace fextl { using string=std::string; }
namespace LogMan::Msg { template<class... T> void DFmt(const char*, const T&...) {} }
static int failure_stage;
static std::string GetConfigDirectory(bool global) { return global ? "/global/" : "C:/local/"; }
namespace FHU::Filesystem {
static bool Exists(const std::string& path) {
  return failure_stage==0 || (failure_stage==2 && path=="C:/local/");
}
static bool CreateDirectories(const std::string&) { return false; }
}
'''
            paths_fixture += paths_source[paths_start:paths_end] + r'''
int main() {
  assert(GetApplicationConfig("vp32-route-probe.exe",true)=="/global/AppConfig/vp32-route-probe.exe.json");
  assert(GetApplicationConfig("RichMan8.exe",false)=="C:/local/AppConfig/RichMan8.exe.json");
  failure_stage=1;
  assert(GetApplicationConfig("War3.exe",false)=="./War3.exe.json");
  failure_stage=2;
  assert(GetApplicationConfig("game.exe",false)=="./game.exe.json");
  const std::string backing="test.exeIGNORED";
  failure_stage=0;
  assert(GetApplicationConfig(std::string_view(backing.data(),8),false)=="C:/local/AppConfig/test.exe.json");
}
'''
            paths_cpp = work / 'paths.cpp'
            paths_cpp.write_text(paths_fixture)
            subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(paths_cpp), '-o', str(work / 'paths')], check=True, capture_output=True)
            subprocess.run([str(work / 'paths')], check=True)
            # Reproduce the old stream reader's end-of-file buffer and ignored FixedSize.
            old = originals[PATHS[0]]
            win = old[old.index('template<typename T>\nstatic bool LoadFileImpl', old.index('\n#else\n')):old.index('\n#endif', old.index('\n#else\n'))]
            fixture = work / 'file.json'
            fixture.write_text('{"Config":{}}\n')
            old_cpp = work / 'old.cpp'
            old_cpp.write_text('#include <fstream>\n#include <span>\n#include <string>\n#include <vector>\n#include <cassert>\nnamespace fextl { using string=std::string; }\n' + win + '\nint main(int argc,char** argv) { std::vector<char> data; char b[4]; assert(LoadFileImpl(data,argv[1],3) && data.size()>3); assert(LoadFileToBuffer(argv[1],b)==0); }\n')
            subprocess.run(['g++', '-std=c++20', '-O2', str(old_cpp), '-o', str(work / 'old')], check=True, capture_output=True)
            subprocess.run([str(work / 'old'), str(fixture)], check=True)


if __name__ == '__main__':
    unittest.main()
