"""Replay every registered Valve overlay and run extracted production font logic."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
options, remaining = parser.parse_known_args()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef unsigned UINT, DWORD, LCID;
typedef uint16_t WCHAR;
typedef intptr_t INT_PTR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define DEFAULT_CHARSET 1
#define SHIFTJIS_CHARSET 128
#define HANGEUL_CHARSET 129
#define SYMBOL_CHARSET 2
#define TCI_SRCCODEPAGE 2
#define TCI_SRCFONTSIG 3
#define GGO_GRAY4_BITMAP 5
#define LOCALE_NAME_MAX_LENGTH 85
#define FIXME(...) ((void)0)
static void reset_env(void) {
    unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); unsetenv("LC_CTYPE"); unsetenv("LANG");
}
'''
FONT_STUBS = r'''
struct { UINT CodePage; } ansi_cp = {1252};
typedef struct { DWORD fsCsb[2]; } FONTSIGNATURE;
typedef struct { UINT ciACP, ciCharset; FONTSIGNATURE fs; } CHARSETINFO;
typedef struct { unsigned char lfCharSet; WCHAR lfFaceName[32]; } LOGFONTW;
struct gdi_font_face { FONTSIGNATURE fs; const WCHAR *file; } chosen;
struct gdi_font_link { FONTSIGNATURE fs; };
static int match_calls;
static BOOL translate_charset_info(DWORD *cp, CHARSETINFO *csi, DWORD type) {
    csi->ciACP = type == TCI_SRCCODEPAGE ? (UINT)(uintptr_t)cp : 932;
    csi->ciCharset = DEFAULT_CHARSET; csi->fs.fsCsb[0] = csi->ciACP == 936 ? 1 : 2;
    return TRUE;
}
static struct gdi_font_face *find_matching_face_by_name(const WCHAR *name,
        const WCHAR *second, const LOGFONTW *lf, FONTSIGNATURE fs, BOOL bitmap, void *unused) {
    (void)name; (void)second; (void)lf; (void)bitmap; (void)unused;
    assert(fs.fsCsb[0] != 0); match_calls++; return &chosen;
}
static const struct gdi_font_link *find_gdi_font_link(const WCHAR *name) { (void)name; return NULL; }
'''
FONT_MAIN = r'''
int main(void) {
    reset_env(); setenv("LANG", "zh_CN.UTF-8", 1);
#ifdef __OHOS__
    assert(ohos_effective_ansi_cp() == 936);
    LOGFONTW lf = {DEFAULT_CHARSET, {0}};
    assert(find_locale_fallback_face(&lf, FALSE) == &chosen); assert(match_calls == 1);
    const unsigned char explicit_charsets[] = {SHIFTJIS_CHARSET,HANGEUL_CHARSET,SYMBOL_CHARSET,0};
    for (unsigned i = 0; i < sizeof(explicit_charsets); i++) {
        lf.lfCharSet = explicit_charsets[i];
        assert(find_locale_fallback_face(&lf, FALSE) == NULL);
    }
    lf.lfCharSet = DEFAULT_CHARSET; lf.lfFaceName[0] = '@';
    assert(find_locale_fallback_face(&lf, FALSE) == NULL); assert(match_calls == 1);
    setenv("LANG", "zh_TW.UTF-8", 1); assert(ohos_effective_ansi_cp() == 950);
    setenv("LANG", "ja_JP.UTF-8", 1); assert(ohos_effective_ansi_cp() == 932);
    setenv("LANG", "ko_KR.UTF-8", 1); assert(ohos_effective_ansi_cp() == 949);
    setenv("LANG", "en_US.UTF-8", 1); assert(ohos_effective_ansi_cp() == 1252);
    setenv("LANG", "de_DE.UTF-8", 1); assert(ohos_effective_ansi_cp() == 1252);
    reset_env(); ansi_cp.CodePage=949; assert(ohos_effective_ansi_cp() == 949);
    ansi_cp.CodePage=1252; assert(ohos_effective_ansi_cp() == 936);
#else
    assert(ohos_effective_ansi_cp() == 1252);
#endif
    setenv("LANG", "zh_CN.UTF-8", 1); CHARSETINFO csi;
    chosen.fs.fsCsb[0] = 4;
    get_nearest_charset(NULL, &chosen, &csi);
#ifdef __OHOS__
    assert(csi.ciACP == 936);
#else
    assert(csi.ciACP == 932); /* Original first-font-signature behavior. */
#endif
    return 0;
}
'''


class WineFontCompatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='wine-font-compat-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        script = (ROOT / 'scripts/build_wine.sh').read_text()
        cls.patches = [ROOT / 'patches/wine' / name for name in re.findall(
            r'ensure_wine_patch "\$SCRIPT_DIR/\.\./patches/wine/([^"]+)"', script)]
        relatives, added = set(), set()
        for patch in cls.patches:
            relatives.update(re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M))
            added.update(re.findall(r'^--- /dev/null\n\+\+\+ b/(.+)$', patch.read_text(), re.M))
        cls.tree = cls.work / 'wine'
        for relative in relatives - added:
            path = cls.tree / relative; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=options.wine_src))
        # Use the actual production direction detector, with stricter no-fuzz replay.
        helper = function(script, 'ensure_wine_patch() {').replace('--batch', '--batch --fuzz=0')
        command = ('set -euo pipefail\nlog() { :; }\n' + helper +
                   '\nWINE_SRC="$1"\nshift\nfor p in "$@"; do ensure_wine_patch "$p" font-test; done\n')
        replay = ['bash', '-c', command, 'replay', str(cls.tree), *map(str, cls.patches)]
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.once = {path: (cls.tree / path).read_bytes() for path in relatives}
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.twice = {path: (cls.tree / path).read_bytes() for path in relatives}

    def compile_run(self, label, source, defines=()):
        path = self.work / (label + '.c'); path.write_text(source)
        binary = self.work / label
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-Wno-unused-variable', *['-D' + item for item in defines],
                        str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_complete_chain_is_idempotent(self):
        self.assertEqual(self.once, self.twice)
        names = [p.name for p in self.patches]
        self.assertEqual([n for n in names if n.startswith(('0023-', '0024-', '0025-', '0026-'))], ['0023-ohos-default-grayscale-font-aa.patch',
            '0024-ohos-scan-prefix-fonts.patch', '0025-ohos-locale-font-fallback.patch',
            '0026-ohos-musl-selected-locale.patch'])
        source = self.once['dlls/win32u/font.c'].decode()
        self.assertEqual(source.count('static UINT ohos_font_aa_override(void)'), 1)
        self.assertNotIn('ohos_skip_foreign_cjk_face', source)

    def test_default_fallback_charset_and_platform_boundaries(self):
        source = self.once['dlls/win32u/font.c'].decode()
        helpers = source[source.index('#ifdef __OHOS__\nstatic BOOL ohos_is_c_unix_locale'):
                         source.index('int add_gdi_face(')]
        fallback = source[source.index('#ifdef __OHOS__\nstatic struct gdi_font_face *find_locale_fallback_face'):
                          source.index('static struct gdi_font_face *find_matching_face(')]
        nearest = function(source, 'static void get_nearest_charset(')
        code = COMMON + FONT_STUBS + helpers + fallback + nearest + FONT_MAIN
        self.compile_run('font-ohos', code, ['__OHOS__'])
        self.compile_run('font-upstream', code)
        self.assertIn('#ifdef __OHOS__\n    /* Missing default-charset names', source)

    def test_grayscale_and_prefix_discovery(self):
        source = self.once['dlls/win32u/freetype.c'].decode()
        helper = '#ifdef __OHOS__\n' + function(source, 'static void load_ohos_windows_fonts(') + '#endif\n'
        code = COMMON + r'''
static int default_aa_flags, fontconfig_enabled, scans;
static char paths[3][PATH_MAX];
static void load_fontconfig_fonts(void) {}
static void ReadFontDir(const char *path, BOOL external) {
    assert(scans < 3); snprintf(paths[scans++], PATH_MAX, "%s", path); (void)external;
}
''' + helper + function(source, 'static void freetype_load_fonts(') + r'''
int main(void) {
    reset_env(); setenv("WINEPREFIX", "/test/prefix", 1);
    freetype_load_fonts();
#ifdef __OHOS__
    assert(default_aa_flags == GGO_GRAY4_BITMAP); assert(scans == 3);
    assert(!strcmp(paths[0], "/system/fonts"));
    assert(!strcmp(paths[1], "/test/prefix/drive_c/windows/fonts"));
    assert(!strcmp(paths[2], "/test/prefix/drive_c/windows/Fonts"));
#ifdef SONAME_LIBFONTCONFIG
    scans=0; default_aa_flags=7; fontconfig_enabled=TRUE; freetype_load_fonts();
    assert(default_aa_flags == 7 && scans == 2);
#endif
#else
    assert(default_aa_flags == 0 && scans == 0);
#endif
    return 0;
}
'''
        self.compile_run('freetype-ohos', code, ['__OHOS__', 'SONAME_LIBFONTCONFIG'])
        self.compile_run('freetype-ohos-no-fontconfig', code, ['__OHOS__'])
        self.compile_run('freetype-upstream', code, ['SONAME_LIBFONTCONFIG'])

    def test_c_utf8_selected_locale_is_ohos_only(self):
        source = self.once['dlls/ntdll/unix/env.c'].decode()
        helper_start = source.index('#ifdef __OHOS__\n/* musl/OHOS')
        helpers = source[helper_start:source.index('/* Unix format is:', helper_start)]
        selection = '#ifdef __OHOS__\n' + function(source, 'static BOOL ohos_env_locale_is(') + function(source, 'static LCID ohos_requested_locale(') + '#endif\n'
        code = COMMON + helpers + function(source, 'static BOOL unix_to_win_locale(') + selection + r'''
int main(void) {
    char name[LOCALE_NAME_MAX_LENGTH]; reset_env();
    setenv("LC_ALL", "C.UTF-8", 1); setenv("LANG", "zh_CN.UTF-8", 1);
    assert(unix_to_win_locale("C.UTF-8", name));
#ifdef __OHOS__
    assert(!strcmp(name, "zh-CN")); assert(ohos_requested_locale(name) == 0x0804);
    setenv("LANG", "zh_TW.UTF-8", 1); assert(ohos_requested_locale(name) == 0x0404);
    setenv("LANG", "ja_JP.UTF-8", 1); assert(ohos_requested_locale(name) == 0x0411);
    setenv("LANG", "en_US.UTF-8", 1); assert(ohos_requested_locale(name) == 0x0409);
    setenv("LANG", "zh_CN_invalid", 1); assert(ohos_requested_locale(name) == 0);
#else
    assert(!strcmp(name, "C")); /* Preserve upstream parsing outside OHOS. */
#endif
    return 0;
}
'''
        self.compile_run('locale-ohos', code, ['__OHOS__'])
        self.compile_run('locale-upstream', code)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
