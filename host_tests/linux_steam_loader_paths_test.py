"""Exercise real GNU dlopen/dlmopen against the signed-code path adapter."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class LoaderPaths(unittest.TestCase):
    def test_absolute_relative_and_unrelated_paths(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-loader-paths-") as temporary:
            base = Path(temporary)
            code = base / "signed/steamrtarm64"
            code.mkdir(parents=True)
            data = base / "data"
            data.mkdir()
            fixture = base / "fixture.c"
            fixture.write_text("int steamohos_probe(void) { return 73; }\n")
            subprocess.run(["cc", "-shared", "-fPIC", str(fixture), "-o", str(code / "probe.so")], check=True)
            runner_source = base / "runner.c"
            runner_source.write_text(r'''#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <link.h>
#include <assert.h>
#include <stdio.h>
int main(int argc, char **argv) {
    assert(argc == 3);
    void *self = dlopen(NULL, RTLD_NOW); assert(self); dlclose(self);
    const char *names[] = {argv[1], "steamrtarm64/probe.so", "./steamrtarm64/probe.so"};
    for (unsigned i = 0; i < 3; ++i) {
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (!handle) puts(dlerror());
        assert(handle);
        int (*probe)(void) = dlsym(handle, "steamohos_probe"); assert(probe && probe() == 73);
        assert(!dlclose(handle));
    }
    void *other = dlmopen(LM_ID_NEWLM, argv[1], RTLD_NOW | RTLD_LOCAL); assert(other); dlclose(other);
    assert(!dlopen(argv[2], RTLD_NOW)); assert(dlerror());
    void *normal = dlopen("libm.so.6", RTLD_NOW); assert(normal); dlclose(normal);
    puts("PASS GNU real dlopen/dlmopen; absolute and relative signed-code paths; unrelated paths and dlerror preserved");
}
''')
            runner = base / "runner"
            subprocess.run(["cc", str(runner_source), "-ldl", "-o", str(runner)], check=True)
            library = base / "compat.so"
            fixture_dir = ROOT / "entry/src/main/cpp/linux_steam/gnu_fixture"
            subprocess.run(["cc", "-O2", "-shared", "-fPIC", str(fixture_dir / "posix_compat.c"),
                            str(fixture_dir / "sysvipc_compat.c"), str(fixture_dir / "filesystem_compat.c"), str(fixture_dir / "syscall_compat.S"), "-pthread", "-ldl", "-o", str(library)], check=True)
            environment = os.environ.copy()
            environment.update(LD_PRELOAD=str(library), STEAMOHOS_STEAM_ROOT=str(data),
                               STEAMOHOS_CODE_ROOT=str(code.parent))
            result = subprocess.run([str(runner), str(data / "steamrtarm64/probe.so"),
                                     str(base / "data-other/steamrtarm64/probe.so")], env=environment,
                                    cwd=base, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('STEAMOHOS_IMAGE', result.stderr)
            print(result.stdout, end="")
            environment['STEAMOHOS_MODULE_DIAGNOSTICS'] = '1'
            observed = subprocess.run([str(runner), str(data / "steamrtarm64/probe.so"),
                                       str(base / "data-other/steamrtarm64/probe.so")], env=environment,
                                      cwd=base, capture_output=True, text=True, timeout=5)
            self.assertEqual(observed.returncode, 0, observed.stdout + observed.stderr)
            self.assertIn('name=<main>', observed.stderr)
            self.assertIn('name=probe.so', observed.stderr)
            images = [line for line in observed.stderr.splitlines() if line.startswith('STEAMOHOS_IMAGE')]
            self.assertTrue(images)
            self.assertTrue(all(str(base) not in line for line in images))

if __name__ == "__main__":
    unittest.main()
