"""Execute the signed GNU handoff with real loader, fork, signals and descriptors."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NativeExec(unittest.TestCase):
    def test_real_rtld_handoff_and_negative_cases(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-exec-", dir="/var/tmp") as temporary:
            base = Path(temporary)
            signed = base / "signed"
            signed.mkdir()
            (signed / "sub").mkdir()
            shutil.copyfile("/bin/bash",signed / "steamohos_shell")
            shutil.copyfile("/usr/bin/taskset",signed / "steamohos_taskset")
            native = ROOT / "entry/src/main/cpp/linux_steam"
            objects = []
            for source in [native / "gnu_fixture/exec_compat.cpp", native / "native_loader.cpp",
                           native / "native_loader_entry.S"]:
                obj = base / (source.name + ".o")
                subprocess.run(["g++", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-fPIC", "-c",
                                str(source), "-o", str(obj)], check=True)
                objects.append(str(obj))
            source = native / "gnu_fixture/popen_compat.c"
            obj = base / "popen_compat.o"
            subprocess.run(["cc", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-fPIC", "-c",
                            str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
            library = signed / "libsteamohos_posix.so"
            subprocess.run(["g++", "-shared", *objects, "-pthread", "-ldl", "-o", str(library)], check=True)
            gamepad_stub=base/"gamepad.c"
            gamepad_stub.write_text('#include <stdlib.h>\n__attribute__((constructor))static void mark(void){setenv("EXEC_FIXTURE_GAMEPAD_BRIDGE","present",1);}')
            subprocess.run(["cc","-shared","-fPIC",str(gamepad_stub),"-o",str(signed/"libsteamohos_gamepad.so")],check=True)
            caller = base / "caller"
            subprocess.run(["cc", str(ROOT / "host_tests/linux_steam_exec_caller.c"),
                            "-pthread", "-o", str(caller)], check=True)
            subprocess.run(["cc", str(ROOT / "host_tests/linux_steam_exec_target.c"),
                            "-o", str(signed / "target")], check=True)
            interpreter = re.search(r"interpreter: (.*?)\]", subprocess.check_output(
                ["readelf", "-l", str(caller)], text=True)).group(1)
            shutil.copyfile(interpreter, signed / Path(interpreter).name)
            for soname in ["libc.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6", "libtinfo.so.6"]:
                source = subprocess.check_output(["g++", "-print-file-name=" + soname], text=True).strip()
                shutil.copyfile(source, signed / soname)
            (signed / "script").write_text("#!/bin/sh\nexit 0\n")
            tool = signed / "compatibilitytools.d/hmosdeck/hmosdeck"
            tool.parent.mkdir(parents=True)
            subprocess.run(["g++", "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                            str(native / "gnu_fixture/hmosdeck.cpp"), "-o", str(tool)], check=True)
            (signed / "steamrtarm64").mkdir()
            reaper_source = base / "reaper.c"
            reaper_source.write_text(r'''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
 assert(argc==5);
 assert(!strcmp(getenv("STEAMOHOS_LAUNCH_SOCKET"),"/var/tmp/exec-fixture.sock"));
 assert(strlen(getenv("STEAMOHOS_LAUNCH_NONCE"))==64);
 assert(!strcmp(getenv("STEAMOHOS_Z_ROOT"),"/var/tmp/exec-fixture-z"));
 assert(!strcmp(getenv("STEAMOHOS_WINE_PREFIX"),"/var/tmp/exec-fixture-wine"));
 assert(!strcmp(getenv("STEAMOHOS_LIBRARY_ROOT"),"/var/tmp/exec-fixture-library"));
 char *target=argv[1]; argv[1]="exec-target";
 execv(target,argv+1);return 127;
}
''')
            subprocess.run(["cc", str(reaper_source), "-o", str(signed / "steamrtarm64/reaper")], check=True)
            wrapper_environment = os.environ.copy()
            wrapper_environment["STEAMOHOS_CODE_ROOT"] = str(signed)
            for args in [["--"], ["--", "/bin/true"], ["--", str(signed / "steamrtarm64/../target")]]:
                self.assertEqual(subprocess.run([str(tool), *args], env=wrapper_environment,
                    capture_output=True, timeout=10).returncode, 64)
            reaper = signed / "steamrtarm64/reaper"
            saved_reaper = signed / "steamrtarm64/reaper-original"
            reaper.rename(saved_reaper)
            reaper.symlink_to("/bin/true")
            self.assertEqual(subprocess.run([str(tool), "--", str(reaper)], env=wrapper_environment,
                capture_output=True, timeout=10).returncode, 64)
            reaper.unlink()
            saved_reaper.rename(reaper)
            environment = os.environ.copy()
            environment.update(LD_PRELOAD=str(library), STEAMOHOS_CODE_ROOT=str(signed),
                               STEAMOHOS_SOCKET_METADATA_ROOT="/var/tmp/steamohos-exec-scope",
                               STEAMOHOS_SYSVIPC_ROOT="/var/tmp/steamohos-exec-sysvipc",
                               EXEC_FIXTURE_MARKER="preserved", PATH=str(signed) + ":" + environment["PATH"],
                               GLIBC_TUNABLES="glibc.pthread.rseq=0", HOME=str(base / "home"),
                               STEAMOHOS_LAUNCH_SOCKET="/var/tmp/exec-fixture.sock",
                               STEAMOHOS_LAUNCH_NONCE="a" * 64,
                               STEAMOHOS_Z_ROOT="/var/tmp/exec-fixture-z",
                                STEAMOHOS_WINE_PREFIX="/var/tmp/exec-fixture-wine",
                                STEAMOHOS_LIBRARY_ROOT="/var/tmp/exec-fixture-library",
                                STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET="/var/tmp/exec-fixture-audio",
                                ALSA_CONFIG_PATH="/var/tmp/exec-fixture-asound.conf",
                                ALSA_PLUGIN_DIR=str(signed),SDL_AUDIO_DRIVER="alsa",
                                STEAMOHOS_GAMEPAD_SOCKET="/var/tmp/exec-fixture-gamepad")
            environment.pop("STEAMOHOS_EXEC_GENERATION", None)
            for testCase in range(26):
                mode = testCase % 13
                memory = testCase >= 13
                environment.update(STEAMOHOS_NATIVE_MEMORY="1" if memory else "0", STEAMOHOS_EXTERNAL_RUNTIME=str(signed))
                with self.subTest(mode=mode, nativeMemory=memory):
                    environment.pop("EXEC_FIXTURE_CPU",None)
                    if mode==10:environment["EXEC_FIXTURE_CPU"]=str(min(os.sched_getaffinity(0)))
                    result = subprocess.run([str(caller), str(signed / "target"), str(signed), str(mode)],
                        env=environment, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS", result.stdout)
                    if mode != 7:
                        self.assertIn("native_handoff", result.stderr)
                    print("mode", mode, result.stdout.strip())

if __name__ == "__main__":
    unittest.main()
