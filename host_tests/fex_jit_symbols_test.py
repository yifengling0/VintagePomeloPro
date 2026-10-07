"""Exercise the patched production JIT symbol path and buffer flush methods."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/fex-windows-jit-symbols.patch'
SOURCE = 'FEXCore/Source/Common/JitSymbols.cpp'
MODULE = 'Source/Windows/WOW64/Module.cpp'


def pinned(path):
    return subprocess.check_output(['git', '-C', str(ROOT / 'thirdparty/fex'),
        'show', '86ff33bbe2:' + path], text=True)


def extract(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


STUBS = r'''
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
static std::string last_path;
static unsigned writes;
static size_t written_bytes;
static int capture_open(const char* path, int flags, int mode) {
  last_path = path;
  assert((flags & (O_CREAT | O_TRUNC | O_WRONLY | O_APPEND)) ==
    (O_CREAT | O_TRUNC | O_WRONLY | O_APPEND));
  assert(mode == 0644);
  return 42;
}
static ssize_t capture_write(int fd, const void*, size_t size) {
  assert(fd == 42); ++writes; written_bytes += size; return size;
}
namespace fextl::fmt {
static std::string format(const char* pattern, const char* directory, int pid) {
  std::string result(pattern);
  result.replace(result.find("{}"), 2, directory);
  result.replace(result.find("{}"), 2, std::to_string(pid));
  return result;
}
static std::string format(const char* pattern, int pid) {
  std::string result(pattern);
  result.replace(result.find("{}"), 2, std::to_string(pid));
  return result;
}
}
namespace FEXCore {
struct JITSymbolBuffer {
  static constexpr size_t NEEDS_WRITE_DISTANCE = 4000;
  static constexpr std::chrono::milliseconds MAXIMUM_THRESHOLD{100};
  std::chrono::steady_clock::time_point LastWrite{std::chrono::steady_clock::now()};
  size_t Offset{};
  char Buffer[4096]{};
};
class JITSymbols {
public:
  int fd{-1};
  void InitFile();
  void WriteBuffer(JITSymbolBuffer*, bool ForceWrite);
};
}
#define open capture_open
#define write capture_write
#define _WIN32 1
'''

MAIN = r'''
#undef _WIN32
#undef write
#undef open
int main(int argc, char**) {
  assert(argc == 1);
  FEXCore::JITSymbols symbols;
  setenv("FEX_JITMAPDIR", "C:\\diagnostics with spaces", 1);
  setenv("TEMP", "C:\\windows\\temp", 1);
  symbols.InitFile();
  auto suffix = "\\perf-" + std::to_string(getpid()) + ".map";
  assert(last_path == "C:\\diagnostics with spaces" + suffix);
  setenv("FEX_JITMAPDIR", "", 1);
  symbols.InitFile();
  assert(last_path == "C:\\windows\\temp" + suffix);
  unsetenv("FEX_JITMAPDIR");
  unsetenv("TEMP");
  symbols.InitFile();
  assert(last_path == "." + suffix);

  FEXCore::JITSymbolBuffer buffer;
  buffer.Offset = 32;
  symbols.WriteBuffer(&buffer, false);
  assert(writes == 0 && buffer.Offset == 32);
  buffer.LastWrite = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
  symbols.WriteBuffer(&buffer, false);
  assert(writes == 1 && written_bytes == 32 && buffer.Offset == 0);
  buffer.Offset = FEXCore::JITSymbolBuffer::NEEDS_WRITE_DISTANCE;
  symbols.WriteBuffer(&buffer, false);
  assert(writes == 2 && buffer.Offset == 0);
  buffer.Offset = 17;
  symbols.WriteBuffer(&buffer, true);
  assert(writes == 3 && buffer.Offset == 0);
  assert(written_bytes == 32 + FEXCore::JITSymbolBuffer::NEEDS_WRITE_DISTANCE + 17);
}
'''


class JITSymbolTests(unittest.TestCase):
    def test_real_methods_and_replay(self):
        original = pinned(SOURCE)
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            for path in (SOURCE, MODULE):
                destination = work / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(pinned(path))
            subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            changed = (work / SOURCE).read_text()
            subprocess.run(['patch', '-p1', '-R', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            self.assertEqual((work / SOURCE).read_text(), original)
            subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            self.assertEqual((work / SOURCE).read_text(), changed)
            methods = extract(changed, 'void JITSymbols::InitFile()') + '\n' + extract(changed, 'void JITSymbols::WriteBuffer(')
            source = work / 'test.cpp'
            source.write_text(STUBS + '\nnamespace FEXCore {\n' + methods + '\n}\n' + MAIN)
            executable = work / 'test'
            subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', str(source), '-o', str(executable)],
                check=True, capture_output=True)
            subprocess.run([str(executable)], check=True)

            # Reproduce the original timed-flush failure with the same fixture.
            old_flush = extract(original, 'void JITSymbols::WriteBuffer(')
            old_main = MAIN[MAIN.index('#undef _WIN32'):MAIN.index('int main')]
            old_main += r'''int main() {
              FEXCore::JITSymbols symbols; symbols.fd = 42;
              FEXCore::JITSymbolBuffer buffer; buffer.Offset = 32;
              buffer.LastWrite = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
              symbols.WriteBuffer(&buffer, false);
              assert(writes == 0 && buffer.Offset == 32);
            }'''
            source.write_text(STUBS + '\nnamespace FEXCore {\n' + old_flush + '\n}\n' + old_main)
            subprocess.run(['g++', '-std=c++20', '-O2', str(source), '-o', str(executable)], check=True, capture_output=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
