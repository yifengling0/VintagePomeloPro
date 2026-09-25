#!/usr/bin/env python3
"""Exercise the pinned Wine locale parser and guard both game launch paths."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function_body(source, name):
    start = source.index("static ", source.rfind("\n", 0, source.index(name)))
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WineLocaleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="wine-locale-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.source = (ROOT / "thirdparty/wine/dlls/ntdll/unix/env.c").read_text()
        functions = ("is_c_unix_locale", "unix_locale_from_env", "ohos_env_locale_is",
                     "ohos_requested_locale")
        code = "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        code += "typedef int BOOL; typedef unsigned int LCID;\n"
        code += "\n".join(function_body(cls.source, name) for name in functions)
        code += '\nint main(void) { char name[128] = ""; unsigned int id = ohos_requested_locale(name); printf("%04x %s", id, name); }\n'
        path = Path(cls.temp.name)
        (path / "locale.c").write_text(code)
        cls.probe = path / "locale"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path / "locale.c"),
                        "-o", str(cls.probe)], check=True)

    def parse(self, values):
        env = {key: value for key, value in os.environ.items()
               if key != "LANG" and not key.startswith("LC_")}
        env.update(values)
        return subprocess.check_output([str(self.probe)], env=env, text=True)

    def test_selected_locales_and_musl_fallback(self):
        for locale, expected in (("zh_TW", "0404 zh-TW"), ("ja_JP", "0411 ja-JP"),
                                 ("zh_CN", "0804 zh-CN"), ("en_US", "0409 en-US")):
            with self.subTest(locale=locale):
                self.assertEqual(self.parse({"LC_ALL": locale + ".UTF-8", "LANG": "zh_CN.UTF-8"}), expected)
                self.assertEqual(self.parse({"LC_ALL": "C.UTF-8", "LANG": locale + ".UTF-8"}), expected)
                self.assertEqual(self.parse({"LC_CTYPE": locale + ".UTF-8"}), expected)
        self.assertEqual(self.parse({"LC_ALL": "zh_TW_invalid"}), "0000 ")
        self.assertEqual(self.parse({"LC_ALL": "C.UTF-8"}), "0000 ")

    def test_system_and_user_follow_selected_locale(self):
        self.assertIn("system_lcid = user_lcid = selected_lcid;", self.source)

    def test_game_launches_preserve_session_language(self):
        service = (ROOT / "entry/src/main/ets/service/WineEngineService.ets").read_text()
        native = (ROOT / "entry/src/main/cpp/wine/wine_exe.cpp").read_text()
        self.assertIn("this.activeWineLanguage !== requestedWineLanguage", service)
        self.assertRegex(service, r"testNapi\.runWineExe\([\s\S]*?launchEnvironment,\s*this\.activeWineLanguage\s*\)")
        self.assertIn('GetString(env, args[0], "wineLang", "zh_CN")', native)
        self.assertIn("policy.wineLang = options.wineLang;", native)
        self.assertIn("policy.wineLang = wineLang;", native)
        self.assertIn("ReadString(env, args[9])", native)


if __name__ == "__main__":
    unittest.main(verbosity=2)
