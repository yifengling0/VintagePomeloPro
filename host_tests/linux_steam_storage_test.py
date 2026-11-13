"""Real directory reset: private Steam remains intact, links are never followed."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class Storage(unittest.TestCase):
    def test_wine_reset_preserves_only_private_steam(self):
        with tempfile.TemporaryDirectory(prefix='steam-storage-') as temporary:
            base=Path(temporary); prefix=base/'prefix'; steam=prefix/'drive_c/SteamLinuxARM64'
            (steam/'library').mkdir(parents=True); game=steam/'library/game'; game.write_bytes(b'game-data')
            identity=game.stat().st_ino
            (prefix/'drive_c/windows').mkdir(); (prefix/'system.reg').write_text('old-registry')
            outside=base/'outside';outside.mkdir();(outside/'save').write_bytes(b'user-save')
            (prefix/'outside-link').symlink_to(outside, target_is_directory=True)
            source=base/'reset.cpp';runner=base/'reset'
            source.write_text('#include "wine/prefix_reset.h"\nint main(int n,char** a){return n==3&&winehua::RemovePrefixTree(a[1],a[2])?0:1;}')
            subprocess.run(['g++','-std=c++17','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-I'+str(ROOT/'entry/src/main/cpp'),str(source),'-o',str(runner)],check=True)
            subprocess.run([str(runner),str(prefix),str(steam)],check=True)
            self.assertEqual(game.read_bytes(),b'game-data');self.assertEqual(game.stat().st_ino,identity)
            self.assertEqual(list(prefix.iterdir()),[prefix/'drive_c'])
            self.assertEqual(list((prefix/'drive_c').iterdir()),[steam])
            self.assertEqual((outside/'save').read_bytes(),b'user-save')
            subprocess.run([str(runner),str(prefix),str(steam)],check=True)
            linked=base/'linked-prefix';linked.symlink_to(outside,target_is_directory=True)
            self.assertNotEqual(subprocess.run([str(runner),str(linked),str(steam)]).returncode,0)
            self.assertEqual((outside/'save').read_bytes(),b'user-save')
if __name__=='__main__':unittest.main()
