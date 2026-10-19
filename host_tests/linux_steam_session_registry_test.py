import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class SteamSessionMarker(unittest.TestCase):
 def test_marker_parser_from_the_actual_wine_overlay(self):
  patch=(ROOT/'patches/wine/0065-kernelbase-linux-steam-session-registry.patch').read_text()
  added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
  function=added[added.index('static BOOL hmosdeck_steam_session_enabled'):added.index('static BOOL hmosdeck_steam_session_key')]
  harness=r'''#include <wchar.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef wchar_t WCHAR; typedef unsigned DWORD; typedef unsigned long long ULONGLONG; typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static const char *marker, *app;
static DWORD GetEnvironmentVariableW(const WCHAR *name, WCHAR *out, DWORD cap) {
 const char *value=!wcscmp(name,L"SteamAppId")?app:marker;
 if(!value)return 0;size_t len=strlen(value);if(len>=cap)return (DWORD)len+1;
 for(size_t i=0;i<=len;++i)out[i]=(unsigned char)value[i];return (DWORD)len;
}
'''+function+r'''
int main(int argc, char **argv) {marker=argc>1?argv[1]:NULL;app=argc>2?argv[2]:NULL;return hmosdeck_steam_session_enabled()?0:1;}
'''
  with tempfile.TemporaryDirectory(prefix='steam-session-marker-') as temp:
   root=pathlib.Path(temp);source=root/'test.c';source.write_text(harness)
   subprocess.run(['cc','-std=c11','-O2',str(source),'-o',str(root/'test')],check=True)
   nonce='0123456789abcdef'*4
   valid=f'32123:{nonce}:695630'
   cases=[([],False),([valid,'695630'],True),([valid,'1'],False),([valid,''],False),([f'1:{nonce}:1','1'],True),([f'65535:{nonce}:4294967295','4294967295'],True)]
   for port in ['0','65536','9999999999','01','-1',' 1','+1']:
    cases.append(([f'{port}:{nonce}:695630','695630'],False))
   for token in [nonce[:-1],nonce+'a',nonce.upper(),'z'*64,'',nonce+':']:
    cases.append(([f'32123:{token}:695630','695630'],False))
   for app in ['0','01','-1','4294967296','9999999999999999999','695630x','']:
    cases.append(([f'32123:{nonce}:{app}',app],False))
   for args,expected in cases:
    with self.subTest(args=args):
     self.assertEqual(subprocess.run([str(root/'test'),*args]).returncode==0,expected)
if __name__=='__main__':unittest.main()
