#!/usr/bin/env python3
"""Run actual Mesa softpipe cache lifecycle/residency tests on a Linux host.

Reuses a configured GNU Mesa build's include/feature flags; does not rebuild or
change the Wine Mesa submodule. A transfer callback supplies deterministic data.
"""
import argparse,json,shlex,subprocess,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--mesa-build',required=True,type=Path)
 p.add_argument('--output-dir',required=True,type=Path)
 a=p.parse_args();build=a.mesa_build.resolve(strict=True);out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
 commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','src/gallium/drivers/softpipe/libsoftpipe.a.p/sp_tex_tile_cache.c.o'],text=True)
 original=shlex.split(commands.splitlines()[-1]);flags=[];skip=False
 for part in original[1:]:
  if skip:skip=False;continue
  if part in ['-MD','-c']:continue
  if part in ['-o','-MF','-MQ']:skip=True;continue
  if part in ['-DNDEBUG','-DUSE_AARCH64_ASM']:continue
  if part.endswith('.c'):continue
  flags.append(part)
 source=ROOT/'host_tests/softpipe_texture_cache_test.c';binary=out/'softpipe_texture_cache_test'
 subprocess.run(['gcc',*flags,'-Werror',str(source),'-o',str(binary)],cwd=build,check=True,timeout=45)
 result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=15,check=True)
 # Run the same actual cache and callbacks under ASan/UBSan as well.
 sanitizer=out/'softpipe_texture_cache_sanitized_test'
 subprocess.run(['gcc',*flags,'-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(source),'-o',str(sanitizer)],cwd=build,check=True,timeout=45)
 checked=subprocess.run([str(sanitizer)],text=True,capture_output=True,timeout=15,check=True)
 (out/'softpipe-cache-host-result.json').write_text(json.dumps(dict(passed=True,asanUbsanPassed=True,stdout=result.stdout,sanitizerStdout=checked.stdout,sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest()),indent=2)+'\n')
 print(result.stdout.strip());print('ASan/UBSan: passed')
if __name__=='__main__':main()
