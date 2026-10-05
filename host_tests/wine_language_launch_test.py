"""Run real locale handoff code, with NAPI/preferences as platform boundaries."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', action='store_true')
parser.add_argument('--baseline-ref', default='0a877ea3724cf02e248d90c413aac92afbb38fd0')
args = parser.parse_args()

def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def read(path, baseline=False):
    if baseline:
        return subprocess.check_output(['git', 'show', args.baseline_ref + ':' + path],
                                       cwd=ROOT, text=True)
    return (ROOT / path).read_text()

native = read('entry/src/main/cpp/bridge/napi_init.cpp', args.baseline)
native = block(native, 'if (argc >= 8) {\n        // 设置页 "Wine 语言"')
env_source = read('entry/src/main/cpp/wine/wine_env.cpp')
locale_helper = block(env_source, 'std::string WineLocaleFor(')
start = env_source.index('    const std::string locale = WineLocaleFor(wineLang);')
locale_lines = env_source[start:env_source.index('    // winegstreamer', start)]
locale_lines = re.sub(r'//[^\n]*', '', locale_lines)

code = r'''
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
struct Params { std::string wineLang = "zh_CN"; };
static int napi_get_value_string_utf8(void*, const char* input, char* output, size_t size, void*) {
    if (input) { std::strncpy(output,input,size-1);output[size-1]=0; }
    return input ? 0 : 1;
}
'''
code += locale_helper + '\n'
code += 'int main(int count,char** values) { Params param; auto* p=&param; void* env=nullptr; '
code += 'int argc=count>2?std::atoi(values[2]):8; const char* args[8]={}; args[7]=count>1?values[1]:nullptr;\n'
code += native + '\n'
code += 'const std::string wineLang=count>3?values[3]:p->wineLang; std::vector<std::string> lines;\n'
code += locale_lines.replace('env.push_back', 'lines.push_back')
code += 'std::cout<<p->wineLang<<"\\n";for(const auto& line:lines)std::cout<<line<<"\\n";}\n'

model = read('entry/src/main/ets/model/AppModels.ets')
enum = block(model, 'export enum WineLanguage')
members = re.findall(r'(\w+)\s*=\s*\'([^\']+)\'', enum)
normalizer = block(model, 'export function normalizeWineLanguage')
normalizer = normalizer.replace('export ', '').replace('value: string | undefined', 'value')
normalizer = normalizer.replace('): WineLanguage', ')')
service = read('entry/src/main/ets/service/WineEnvService.ets', args.baseline)
setter = block(service, 'setWineLangPreference(lang: string): void')
setter = setter.replace('setWineLangPreference(lang: string): void', 'function setWineLangPreference(lang)')
load = re.search(r'const savedLang = this\.settingsStore\.getSync\([^;]+;\s*'
                 r'this\.wineLang = [^;]+;', service).group(0).replace(' as string', '')
script = "const assert=require('assert');const hilog={info(){},error(){}};const DOMAIN=0;\n"
script += 'const WineLanguage=' + str(dict(members)) + ';\n' + normalizer + '\n' + setter + '\n'
script += 'function loadPreference() {' + load + '}\n'
script += r'''
for(const lang of ['zh_CN','zh_TW','ja_JP','en_US','unknown','',undefined]) {
    const values=new Map();let flushes=0;
    const store={putSync(k,v){values.set(k,v)},flush(){flushes++},getSync(k,d){return values.has(k)?values.get(k):d}};
    const first={settingsStore:store,wineLang:'zh_CN',notify(){}};
    setWineLangPreference.call(first,lang);
    const expected=normalizeWineLanguage(lang);
    assert.equal(first.wineLang,expected);assert.equal(values.get('lang'),expected);assert.equal(flushes,1);
    const restarted={settingsStore:store,wineLang:'zh_CN'};loadPreference.call(restarted);
    assert.equal(restarted.wineLang,expected);
    values.set('lang',lang);loadPreference.call(restarted);assert.equal(restarted.wineLang,expected);
}
const unavailable={settingsStore:null,wineLang:'zh_CN',notify(){}};
setWineLangPreference.call(unavailable,'ja_JP');assert.equal(unavailable.wineLang,'ja_JP');
console.log('Preferences selection, persistence, reload and invalid-value fallback PASS');
'''

with tempfile.TemporaryDirectory(prefix='wine-language-launch-') as folder:
    temp = Path(folder)
    (temp / 'launch.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(temp / 'launch.cpp'),
                    '-o', str(temp / 'launch')], check=True)
    for lang in ('zh_CN', 'zh_TW', 'ja_JP', 'en_US', 'unknown', '', 'ja_JP.UTF-8'):
        selected = lang if lang in ('zh_CN', 'zh_TW', 'ja_JP', 'en_US') else 'zh_CN'
        expected = [selected, 'LANG=' + selected + '.UTF-8', 'LC_ALL=' + selected + '.UTF-8']
        actual = subprocess.check_output([str(temp / 'launch'), lang], text=True).splitlines()
        if args.baseline and lang in ('ja_JP', 'zh_TW'):
            assert actual[0] == 'zh_CN' and actual != expected
            print('BASELINE RED:', lang, 'is silently replaced by zh_CN in native session launch')
        else:
            assert actual == expected, (lang, actual, expected)
    assert subprocess.check_output([str(temp / 'launch')], text=True).splitlines()[0] == 'zh_CN'
    assert subprocess.check_output([str(temp / 'launch'), 'ja_JP', '7'], text=True).splitlines()[0] == 'zh_CN'
    for lang in ('zh_CN', 'zh_TW', 'ja_JP', 'en_US', 'unknown', 'ja_JP.UTF-8', ''):
        selected = lang if lang in ('zh_CN', 'zh_TW', 'ja_JP', 'en_US') else 'zh_CN'
        emitted = subprocess.check_output([str(temp / 'launch'), 'zh_CN', '8', lang], text=True).splitlines()[1:]
        assert emitted == ['LANG=' + selected + '.UTF-8', 'LC_ALL=' + selected + '.UTF-8']
    (temp / 'preferences.js').write_text(script)
    node = shutil.which('node') or '/apps/harmony/tool/node/bin/node'
    result = subprocess.run([node, str(temp / 'preferences.js')], capture_output=True, text=True)
    if args.baseline:
        assert result.returncode != 0 and 'AssertionError' in result.stderr, result.stderr + result.stdout
        print('BASELINE RED: stored traditional/Japanese selection collapses to zh_CN')
    else:
        assert result.returncode == 0, result.stderr
        print(result.stdout.strip())
        print('Native desktop bridge and real LANG/LC_ALL emission PASS for all four languages')
