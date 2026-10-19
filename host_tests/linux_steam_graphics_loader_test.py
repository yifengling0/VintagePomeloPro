"""An isolated constructor failure must produce a real fault record, not PASS."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GraphicsLoader(unittest.TestCase):
    def test_real_load_and_constructor_fault(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-graphics-loader-") as temporary:
            base = Path(temporary)
            runner = base / "probe"
            source = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture/graphics_loader.c"
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", str(source),
                            "-ldl", "-o", str(runner)], check=True)
            environment = os.environ.copy()
            environment.update(LD_LIBRARY_PATH=str(base), STEAMOHOS_TMP_ROOT=str(base))
            for crashes in (False, True):
                library = base / "vulkan.c"
                library.write_text(
                    '#include <signal.h>\n'
                    'void *vkGetInstanceProcAddr(void) { return 0; }\n' +
                    ('__attribute__((constructor)) static void initialize(void) { raise(SIGSEGV); }\n'
                     if crashes else ''))
                subprocess.run(["cc", "-shared", "-fPIC", str(library),
                                "-o", str(base / "libvulkan.so.1")], check=True)
                result = subprocess.run([str(runner)], env=environment, capture_output=True,
                                        text=True, timeout=5)
                self.assertIn("relative=0/0 absolute=0/0 raw=0/0", result.stderr)
                if crashes:
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("STEAMOHOS_LOADER_FAULT signal=0xb", result.stderr)
                    self.assertIn("pc=0x", result.stderr)
                    self.assertIn("name=" + str(base / "libvulkan.so.1"), result.stderr)
                    self.assertIn("child_exit=139", result.stderr)
                    self.assertNotIn("loaded=1", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("loaded=1", result.stderr)
                    self.assertNotIn("STEAMOHOS_LOADER_FAULT", result.stderr)
            print("PASS real shared-library load, constructor SIGSEGV isolation, fault PC and image identity")


if __name__ == "__main__":
    unittest.main()
