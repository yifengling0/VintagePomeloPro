import os, pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class LaunchTest(unittest.TestCase):
    def test_prefix_paths_without_starting_wine(self):
        with tempfile.TemporaryDirectory(prefix='hmosdeck-paths-') as temporary:
            build=pathlib.Path(temporary);prefix=build/'prefix';zroot=build/'download'
            documents=prefix/'drive_c/users/steamuser/Documents';documents.mkdir(parents=True)
            zroot.mkdir()
            source=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/hmosdeck.cpp'
            subprocess.run(['g++','-std=c++17','-O2','-DNDEBUG','-Wall','-Wextra','-Werror',str(source),'-o',str(build/'tool')],check=True)
            env={**os.environ,'STEAMOHOS_Z_ROOT':str(zroot),'STEAMOHOS_WINE_PREFIX':str(prefix)}
            def convert(verb,path):
                return subprocess.run([str(build/'tool'),verb,path],env=env,capture_output=True,text=True)
            win='C:\\users\\steamuser\\Documents\\My Games\\NewPAL'
            native=str(documents/'My Games/NewPAL')
            self.assertEqual(convert('getnativepath',win).stdout.strip(),native)
            self.assertEqual(convert('getnativepath',win.upper()).stdout.strip(),str(documents/'MY GAMES/NEWPAL'))
            self.assertEqual(convert('getcompatpath',native).stdout.strip(),win)
            self.assertEqual(convert('getnativepath','Z:\\SteamLinuxARM64\\library').stdout.strip(),str(zroot/'SteamLinuxARM64/library'))
            for path in ['C:\\..\\outside','X:\\unknown','C:relative','\\\\server\\share']:
                self.assertEqual(convert('getnativepath',path).returncode,64)
            self.assertEqual(convert('getcompatpath',str(build/'elsewhere')).returncode,64)

    def test_real_native_broker_and_tool(self):
        with tempfile.TemporaryDirectory(prefix='hmosdeck-launch-host-') as temporary:
            build=pathlib.Path(temporary)
            source=ROOT/'entry/src/main/cpp'
            common=['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(source)]
            subprocess.run([*common,str(source/'linux_steam/gnu_fixture/hmosdeck.cpp'),'-o',str(build/'tool')],check=True)
            # Keep the harness assertions; compile the production broker as Release.
            subprocess.run([*common,'-DNDEBUG','-c',str(source/'linux_steam/launch_broker.cpp'),'-o',str(build/'broker.o')],check=True)
            subprocess.run([*common,str(ROOT/'host_tests/linux_steam_launch_test.cpp'),str(build/'broker.o'),'-pthread','-o',str(build/'test')],check=True)
            compat=build/'compat.so'
            subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',str(source/'linux_steam/gnu_fixture/filesystem_compat.c'),'-ldl','-o',str(compat)],check=True)
            subprocess.run([str(build/'test'),str(build/'tool'),str(compat)],check=True,timeout=20)
if __name__ == '__main__': unittest.main()
