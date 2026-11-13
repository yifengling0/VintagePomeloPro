"""Ensure production Spawner forwards secret arguments/env without logging them."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include "proc/spawner.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
static std::string logs;
static int desiredPid;
void TestLog(const char* format, ...) {
    std::string sanitized = format;
    for (size_t pos = 0; (pos = sanitized.find("%{public}", pos)) != std::string::npos;)
        sanitized.replace(pos, 9, "%");
    char line[2048]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), sanitized.c_str(), args); va_end(args);
    logs += line;
}
pid_t SpawnViaBroker(const std::string& bin, const std::vector<std::string>& argv, const std::vector<std::string>& env) {
    assert(bin == "/runtime/bin");
    assert(argv[argv.size() - 2] == "C:\\secret-path\\game.exe");
    assert(argv.back() == "--auth=secret-command");
    assert(env.size() == 1 && env[0] == "TOKEN=secret-env");
    return desiredPid;
}
int main() {
    winehua::Spawner::ConfigureSession("/secret-home", "/runtime/bin");
    for (const auto kind : {winehua::SpawnKind::WineExe, winehua::SpawnKind::DesktopShell}) {
        winehua::SpawnRequest request{};
        request.kind = kind; request.argv = {"C:\\secret-path\\game.exe", "--auth=secret-command"};
        request.env = {"TOKEN=secret-env"};
        for (const int result : {77, -1}) {
            desiredPid = result; logs.clear();
            assert(winehua::Spawner::Spawn(request) == result);
            assert(logs.find("secret-") == std::string::npos);
            assert(logs.find("paramsBytes=") != std::string::npos);
            assert(logs.find("argvCount=2") != std::string::npos);
            assert(logs.find("envCount=1") != std::string::npos);
        }
    }
    puts("spawn logs: argv/env preserved; success and failure logs contain only metadata");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='spawn-log-test-') as directory:
        folder = Path(directory)
        (folder / 'hilog').mkdir()
        (folder / 'wine').mkdir()
        (folder / 'hilog/log.h').write_text('''#define LOG_APP 0
void TestLog(const char*, ...);
#define OH_LOG_INFO(tag, ...) TestLog(__VA_ARGS__)
#define OH_LOG_ERROR(tag, ...) TestLog(__VA_ARGS__)
''')
        (folder / 'wine/wine_constants.h').write_text('')
        (folder / 'wine/wine_exe.h').write_text('''#include <string>
#include <vector>
#include <sys/types.h>
pid_t SpawnViaBroker(const std::string&, const std::vector<std::string>&, const std::vector<std::string>&);
''')
        path = folder / 'test.cpp'
        path.write_text(TEST)
        binary = folder / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-include', 'sys/types.h', '-I' + str(folder),
                        '-I' + str(ROOT / 'entry/src/main/cpp'),
                        str(ROOT / 'entry/src/main/cpp/proc/spawner.cpp'), str(path),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
