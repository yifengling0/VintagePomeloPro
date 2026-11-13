"""Real Xlib title conversion with missing and packaged XLC databases."""
from pathlib import Path
import importlib.util
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
XVFB = shutil.which('Xvfb')
SHARE = Path(os.environ.get('STEAMOHOS_HOST_X11_SHARE', '/usr/share'))


@unittest.skipUnless(XVFB and (SHARE / 'X11/locale/C/XLC_LOCALE').is_file(), 'Xvfb and standard X11 data required')
class XlibLocale(unittest.TestCase):
    def test_missing_database_and_real_packaged_title_conversion(self):
        with tempfile.TemporaryDirectory(prefix='gnu-xlocale-', dir='/var/tmp') as temporary:
            base = Path(temporary)
            spec = importlib.util.spec_from_file_location('xwayland_builder', ROOT / 'scripts/build_linux_steam_xwayland.py')
            builder = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(builder)
            archive = base / 'x11.zip'
            builder.package_x11_data(SHARE.parent, archive)
            with zipfile.ZipFile(archive) as package:
                self.assertIn('xkb-data/rules/evdev', package.namelist())
                self.assertIn('x11-locale/C/XLC_LOCALE', package.namelist())
                self.assertIn('x11-locale/en_US.UTF-8/XLC_LOCALE', package.namelist())
                package.extractall(base / 'data')
            (base / 'probe.c').write_text(r'''#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <locale.h>
#include <wchar.h>
#include <stdio.h>
int main(void) {
    if(!setlocale(LC_ALL,""))return 3;
    Display *d=XOpenDisplay(NULL);if(!d)return 2;
    wchar_t *names[]={L"Steam",L"Steam\u6e38\u620f\u5e93"};
    for(int i=0;i<2;i++) {
        XTextProperty p={0};int result=XwcTextListToTextProperty(d,names+i,1,XUTF8StringStyle,&p);
        printf("title=%d supported=%d result=%d allocated=%d bytes=%lu\n",i,XSupportsLocale(),result,p.value!=NULL,p.nitems);
        if(p.value)XFree(p.value);
    }
    XCloseDisplay(d);return 0;
}
''')
            (base / 'deny.c').write_text(r'''#define _GNU_SOURCE 1
#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
// Model the absent system X11 tree without changing Xlib's conversion APIs.
#define DENY(name) FILE *name(const char*p,const char*m) { \
    FILE*(*next)(const char*,const char*)=dlsym(RTLD_NEXT,#name); \
    if(!strncmp(p,"/usr/share/X11/locale",21)){errno=ENOENT;return NULL;}return next(p,m); }
DENY(fopen)
DENY(fopen64)
''')
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(base / 'probe.c'), '-lX11', '-o', str(base / 'probe')], check=True)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', str(base / 'deny.c'), '-ldl', '-o', str(base / 'deny.so')], check=True)
            number = 4000 + os.getpid() % 1000
            for _ in range(32):
                with socket.socket(socket.AF_UNIX) as reservation:
                    try:
                        reservation.bind('\0/tmp/.X11-unix/X' + str(number))
                        break
                    except OSError:
                        number += 1
            else:
                self.fail('No isolated X11 display available')
            server = subprocess.Popen([XVFB, ':' + str(number), '-screen', '0', '800x600x24', '-nolisten', 'tcp', '-pn', '-ac'],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and server.poll() is None:
                    with socket.socket(socket.AF_UNIX) as connection:
                        try:
                            connection.connect('\0/tmp/.X11-unix/X' + str(number))
                            break
                        except OSError:
                            time.sleep(0.05)
                else:
                    self.fail('Xvfb readiness timeout')
                environment = os.environ.copy()
                environment.update(DISPLAY=':' + str(number), LD_PRELOAD=str(base / 'deny.so'), LC_ALL='C')
                for supplied in [False, True]:
                    environment['XLOCALEDIR'] = str(base / 'data/x11-locale' if supplied else base / 'absent')
                    result = subprocess.run([str(base / 'probe')], env=environment, capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = 'title=0 supported=1 result=0 allocated=1 bytes=5' if supplied else 'title=0 supported=0 result=-2 allocated=0 bytes=0'
                    self.assertIn(expected, result.stdout)
                environment['LC_ALL'] = 'C.UTF-8'
                result = subprocess.run([str(base / 'probe')], env=environment, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('title=1 supported=1 result=0 allocated=1 bytes=14', result.stdout)
            finally:
                server.terminate()
                server.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
