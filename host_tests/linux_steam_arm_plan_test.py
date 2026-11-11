#!/usr/bin/env python3
"""Verify direct ARM64 plans against real files, without executing foreign ELF."""
import os, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class ArmPlan(unittest.TestCase):
 def test_route_format_scope_and_runtime_contract(self):
  with tempfile.TemporaryDirectory(prefix='vpp-arm-plan-') as temp:
   base=Path(temp);common=base/'library/steamapps/common';game=common/'Game';game.mkdir(parents=True)
   runtime=common/'SteamLinuxRuntime_soldier';runtime.mkdir()
   entry=runtime/'_v2-entry-point';entry.touch()
   runtime2=common/'SteamLinuxRuntime_sniper';runtime2.mkdir();entry2=runtime2/'_v2-entry-point';entry2.touch()
   for name,machine,elfclass in [('arm',183,2),('x64',62,2),('x86',3,1)]:
    (game/name).write_bytes(b'\x7fELF'+bytes([elfclass,1])+bytes(12)+machine.to_bytes(2,'little'))
   (game/'win').write_bytes(b'MZ'+bytes(128));(game/'script').write_text('#!/bin/sh\nexec game\n')
   (base/'outside').write_bytes((game/'arm').read_bytes());(game/'escaped').symlink_to(base/'outside')
   code=base/'plan.cpp';code.write_text('#include "native_fex.h"\n#include <cstdio>\nint main(int argc,char**argv){steamohos::fex::Plan p;bool ok=steamohos::fex::MakeArmPlan(argc-1,argv+1,p);printf("%d %d %s\\n",ok,p.native,p.error.c_str());return ok?0:1;}')
   binary=base/'planner';subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'),str(code),'-o',str(binary)],check=True)
   env=os.environ|{'STEAMOHOS_LIBRARY_ROOT':str(base/'library'),'STEAMOHOS_STEAM_ROOT':'','STEAM_COMPAT_INSTALL_PATH':str(game)}
   def run(args,expected):
    result=subprocess.run([binary,*map(str,args)],env=env,text=True,capture_output=True)
    self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
   run([game/'arm'],0)
   run([entry,'--verb=run','--',game/'arm','argument with spaces'],0)
   run([entry,'--verb=waitforexitandrun','--',entry2,'--verb=run','--',game/'arm'],0)
   for name in ('x64','x86','win','script','escaped'):
    run([entry,'--verb=run','--',game/name],1)
   run([entry,'--verb=run','--',base/'outside'],1)
   run([entry,'--verb=invalid','--',game/'arm'],1)
   run([entry,'--verb=run',game/'arm'],1)
   run([entry,'--verb=run','--',entry2,'--verb=run','--'],1)
if __name__=='__main__':unittest.main()
