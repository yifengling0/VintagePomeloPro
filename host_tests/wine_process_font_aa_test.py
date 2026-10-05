"""Replay manual NCP launch defaults through the actual Valve font override."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
parser.add_argument('--baseline-ref', default='0a877ea3724cf02e248d90c413aac92afbb38fd0')
args = parser.parse_args()

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

child = (ROOT / 'entry/src/main/cpp/proc/wine_child.cpp').read_text()
font = (ROOT / 'thirdparty/wine-valve/dlls/win32u/font.c').read_text()
setup = function(child, 'void NativeChildProcess_OnConnect(') if 'void NativeChildProcess_OnConnect(' in child else child
assert setup.index('apply_entry_param_env_overrides(envOverrides);') < setup.index('apply_process_font_aa_default(argc, argv);')
assert setup.index('apply_process_font_aa_default(argc, argv);') < setup.index('dlopen("ntdll.so"')
if args.baseline:
    child = subprocess.check_output(['git', 'show', args.baseline_ref + ':entry/src/main/cpp/proc/wine_child.cpp'], cwd=ROOT, text=True)
    assert 'apply_process_font_aa_default' not in child
start = child.index('    // Step B: entryParams 中的环境覆盖应用。')
launch_env = child[start:child.index('    const char* vulkanBackend', start)]
code = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <strings.h>
typedef unsigned UINT;
#define __OHOS__ 1
#define GGO_BITMAP 1
#define GGO_GRAY4_BITMAP 5
#define LOG_APP 0
#define OH_LOG_INFO(...) ((void)0)
#define OH_LOG_WARN(...) ((void)0)
#define TRACE(...) ((void)0)
'''
if not args.baseline:
    code += function(child, 'static const char *basename_of_path(')
    code += function(child, 'static std::string trim_quotes(')
    code += function(child, 'static void apply_process_font_aa_default(')
code += function(child, 'static void apply_entry_param_env_overrides(')
code += function(font, 'static UINT ohos_font_aa_override(void)')
code += r'''
int main(int argc,char** argv) {
    assert(argc>=3);
    unsetenv("WINEHUA_FONT_AA");
    std::vector<std::string> envOverrides;
    if (strcmp(argv[1],"(unset)")) envOverrides.emplace_back(std::string("WINEHUA_FONT_AA=")+argv[1]);
    auto launch=[&](int argc,char** argv) {
'''
code += '        (void)argc;(void)argv;\n' + launch_env
code += r'''
    };
    launch(argc-2,argv+2);
    const char* selected=getenv("WINEHUA_FONT_AA");
    printf("%s %u\n",selected?selected:"(unset)",ohos_font_aa_override());
}
'''
with tempfile.TemporaryDirectory(prefix='wine-process-font-aa-') as folder:
    root=Path(folder);(root/'probe.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(root/'probe.cpp'),'-o',str(root/'probe')],check=True)
    def run(value,*argv):
        return subprocess.check_output([str(root/'probe'),value,*argv],text=True).strip()
    for argv in (['wine',r'Z:\游戏\梦幻端午补丁版\启动器.exe'],
                 [r'Z:\游戏\梦幻端午补丁版\启动器.exe'],
                 ['wine',r'"Z:\my game\启动器.exe"'],
                 ['wine','cmd.exe','/c','launch.bat']):
        result=run('(unset)',*argv)
        if args.baseline:
            assert result=='(unset) 0',result
        else:
            assert result=='bitmap 1',result
        for explicit,expected in (('bitmap','bitmap 1'),('gray','gray 5'),('',' 0'),('invalid','invalid 0')):
            assert run(explicit,*argv)==expected.strip(),(explicit,argv)
    for program in ('explorer','EXPLORER.EXE','wineboot','wineboot.exe','wineserver',
                    'services.exe','winedevice.exe','rpcss.exe','plugplay.exe',
                    'svchost.exe','conhost.exe','winemenubuilder.exe'):
        assert run('(unset)','wine','C:/windows/system32/'+program)=='(unset) 0',program
        assert run('gray','wine',program)=='gray 5',program
    assert run('(unset)','wine')=='(unset) 0'
print('BASELINE RED: manual game processes lack bitmap; real Wine override returns 0' if args.baseline else
      'Manual game / BAT bitmap default, actual Wine GGO_BITMAP selection, explicit overrides and desktop helpers PASS')
