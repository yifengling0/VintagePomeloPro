"""Exercise permission preflight and real GNU handoff on non-executable storage."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExecAccess(unittest.TestCase):
    def test_preflight_agrees_with_actual_loader_and_preserves_denials(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-exec-access-", dir="/var/tmp") as temporary:
            base = Path(temporary)
            code, data = base / "code", base / "client"
            for directory in [code / "steamrtarm64", data / "steamrtarm64", base / "home/.steam"]:
                directory.mkdir(parents=True, exist_ok=True)
            native = ROOT / "entry/src/main/cpp/linux_steam"
            objects = []
            for relative in ["gnu_fixture/filesystem_compat.c", "gnu_fixture/exec_compat.cpp",
                             "native_loader.cpp", "native_loader_entry.S"]:
                source = native / relative
                obj = base / (source.name + ".o")
                subprocess.run(["cc" if source.suffix == ".c" else "g++", "-O2", "-Wall", "-Wextra",
                                "-Werror", "-fPIC", "-c", str(source), "-o", str(obj)], check=True)
                objects.append(str(obj))
            library = code / "libsteamohos_posix.so"
            subprocess.run(["g++", "-shared", *objects, "-pthread", "-ldl", "-o", str(library)], check=True)
            target_source = base / "target.c"
            target_source.write_text("int main(void) { return 73; }\n")
            target = code / "steamrtarm64/probe"
            subprocess.run(["cc", str(target_source), "-o", str(target)], check=True)
            target.chmod(0o644)
            # Data resources may contain a copy, but rtld must use the code
            # image: OHOS executable mapping does not cover the client tree.
            shutil.copyfile(target, data / "steamrtarm64/data-probe")
            (data / "steamrtarm64/data-probe").chmod(0o644)
            shutil.copyfile(target, code / "steamrtarm64/data-probe")
            (code / "steamrtarm64/data-probe").chmod(0o644)
            shutil.copyfile(target, data / "steamrtarm64/data-only")
            (data / "steamrtarm64/data-only").chmod(0o644)
            (code / "script").write_text("#!/bin/sh\nexit 0\n")
            shutil.copyfile(target, base / "outside")
            (base / "outside").chmod(0o644)
            broken = bytearray(target.read_bytes())
            broken[18:20] = b"\x03\x00"
            (code / "wrong-architecture").write_bytes(broken)
            broken[18:20] = target.read_bytes()[18:20]
            broken[24:32] = bytes(8)
            (code / "no-entry").write_bytes(broken)
            shutil.copyfile(target, code / "unreadable")
            (code / "unreadable").chmod(0)
            (data / "read-only").mkdir(mode=0o555)
            runner_source = base / "runner.c"
            runner_source.write_text(r'''
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static void denied(const char *path) { assert(access(path,X_OK)==-1); }
int main(int argc,char **argv) {
 assert(argc==2);const char *code=getenv("STEAMOHOS_CODE_ROOT"),*data=getenv("STEAMOHOS_STEAM_ROOT");
 char path[4096],alias[4096],loader[4096],backup[4096];
 snprintf(path,sizeof(path),"%s/steamrtarm64/probe",code);assert(!access(path,R_OK|X_OK));
 snprintf(path,sizeof(path),"%s/steamrtarm64/probe",data);assert(!access(path,X_OK));
 snprintf(alias,sizeof(alias),"%s/.steam/root/steamrtarm64/probe",getenv("HOME"));assert(!access(alias,X_OK));
 int fd=open(data,O_RDONLY|O_DIRECTORY);assert(fd>=0);
 assert(!faccessat(fd,"steamrtarm64/probe",R_OK|X_OK,0));
 assert(!faccessat(fd,"steamrtarm64/probe",R_OK|X_OK,AT_EACCESS));
 assert(faccessat(fd,"steamrtarm64/probe",X_OK,0x40000000)==-1&&errno==EINVAL);
 assert(!close(fd));
 snprintf(path,sizeof(path),"%s/steamrtarm64/data-only",data);denied(path);
 for(int i=0;i<5;++i) {
  const char *leaf[]={"missing","script","wrong-architecture","no-entry","steamrtarm64"};
  snprintf(path,sizeof(path),"%s/%s",code,leaf[i]);
  // Real executable directory traversal is still accepted by the kernel.
  if(i==4)assert(!access(path,X_OK));else denied(path);
 }
 snprintf(path,sizeof(path),"%s/../outside",code);denied(path);
 if(getuid()!=0) {
  snprintf(path,sizeof(path),"%s/unreadable",code);denied(path);
  assert(!fopen(path,"w")&&errno==EACCES);
  char source[4096];snprintf(source,sizeof(source),"%s/script",code);
  snprintf(path,sizeof(path),"%s/read-only/save.tmp",data);
  assert(rename(source,path)==-1&&errno==EACCES);
  assert(!access(source,R_OK));
 }
 snprintf(loader,sizeof(loader),"%s/%s",code,argv[1]);
 snprintf(backup,sizeof(backup),"%s/loader-disabled",code);assert(!rename(loader,backup));
 snprintf(path,sizeof(path),"%s/steamrtarm64/probe",code);denied(path);assert(!rename(backup,loader));
 // Preflight success must correspond to actual fork -> GNU loader -> main,
 // through both code-only and client-side entries with no executable bits.
 const char *entries[]={alias,"steamrtarm64/data-probe"};
 for(int i=0;i<2;++i) {
  pid_t child=fork();assert(child>=0);
  if(!child) { assert(!chdir(data));execl(entries[i],"probe",(char*)NULL);_exit(111); }
  int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==73);
 }
 puts("PASS real GNU exec after X_OK preflight; aliases, dirfd, data/code, missing loader and negative permissions");
}
''')
            runner = base / "runner"
            subprocess.run(["cc", str(runner_source), "-o", str(runner)], check=True)
            interpreter = re.search(r"interpreter: (.*?)\]", subprocess.check_output(
                ["readelf", "-l", str(runner)], text=True)).group(1)
            shutil.copyfile(interpreter, code / Path(interpreter).name)
            for soname in ["libc.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libm.so.6"]:
                source = subprocess.check_output(["g++", "-print-file-name=" + soname], text=True).strip()
                shutil.copyfile(source, code / soname)
            env = {**os.environ, "LD_PRELOAD": str(library), "HOME": str(base / "home"),
                   "STEAMOHOS_CODE_ROOT": str(code), "STEAMOHOS_STEAM_ROOT": str(data)}
            run = subprocess.run([str(runner), Path(interpreter).name], env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("user_loader=1", run.stderr)
            if os.getuid() != 0:
                self.assertRegex(run.stderr, r"STEAMOHOS_FS_FAILURE .*op=fopen .*errno=13")
                self.assertRegex(run.stderr, r"STEAMOHOS_FS_FAILURE .*op=rename .*errno=13")
            print(run.stdout, end="")


if __name__ == "__main__":
    unittest.main()
