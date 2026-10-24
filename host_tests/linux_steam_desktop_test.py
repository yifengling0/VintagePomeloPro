"""Verify the native desktop observer against a real, isolated X server."""
from pathlib import Path
import os
import json
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
XVFB = os.environ.get('STEAMOHOS_HOST_XVFB') or shutil.which('Xvfb')

CLIENT = r'''#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(void) {
    Display *d=XOpenDisplay(NULL);if(!d)return 4;
    Window w=XCreateSimpleWindow(d,DefaultRootWindow(d),20,20,100,80,0,0,0x123456);
    XStoreName(d,w,"Native desktop observer fixture");XSelectInput(d,w,KeyPressMask);
    XMapWindow(d,w);XSync(d,False);XSetInputFocus(d,w,RevertToParent,CurrentTime);XFlush(d);
    if(getenv("HMOSDECK_BOUNDS_FIXTURE")) {
        XSetWindowAttributes attrs={.override_redirect=True};
        Window menu=XCreateWindow(d,DefaultRootWindow(d),790,590,100,80,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect,&attrs);
        XMapWindow(d,menu);XMoveResizeWindow(d,w,900,700,1000,900);XSync(d,False);usleep(1000000);
        XWindowAttributes a,m;XGetWindowAttributes(d,w,&a);XGetWindowAttributes(d,menu,&m);
        int valid=a.x==0&&a.y==0&&a.width==800&&a.height==600&&m.x==790&&m.y==590;
        XMoveWindow(d,w,-60,-40);XSync(d,False);usleep(500000);XGetWindowAttributes(d,w,&a);
        valid=valid&&a.x==0&&a.y==0;
        printf("CLIENT_BOUNDS constrained=%d override_preserved=%d\n",valid,m.x==790&&m.y==590);
        XDestroyWindow(d,menu);XDestroyWindow(d,w);XCloseDisplay(d);return valid?37:10;
    }
    char text[32]={0};unsigned used=0;time_t deadline=time(NULL)+14;
    while(time(NULL)<deadline) {
        while(XPending(d)) {
            XEvent e;XNextEvent(d,&e);
            if(e.type==KeyPress&&used<sizeof(text)-1) {
                int n=XLookupString(&e.xkey,text+used,sizeof(text)-1-used,NULL,NULL);
                if(n>0)used+=n;
            }
        }
        usleep(10000);
    }
    printf("CLIENT_INPUT text=%s\n",text);
    XDestroyWindow(d,w);XCloseDisplay(d);return strcmp(text,"hmosdeck")?9:37;
}
'''


class DesktopConfiguration(unittest.TestCase):
    def test_bounded_launch_settings(self):
        with tempfile.TemporaryDirectory(prefix='gnu-desktop-settings-') as temporary:
            base = Path(temporary)
            source, runner = base / 'settings.cpp', base / 'settings'
            implementation = ROOT / 'entry/src/main/cpp/linux_steam/xwayland_probe.cpp'
            source.write_text('#include "' + str(implementation) + '"\n' + r'''
namespace steamohos {
NativeExecResult RunNativeLoader(const std::vector<std::string>&,
    const std::vector<std::string>&,uint32_t,const NativeExecOptions&) {return {};}
std::string NativeExecJson(const NativeExecResult&) {return "{}";}
std::string JsonString(const std::string& value) {return value;}
}
int main(int argc,char **argv) {
    if(argc==3&&std::string(argv[1])=="cleanup") {steamohos::CleanupSocketMetadata(argv[2]);return 0;}
    if(argc==3&&std::string(argv[1])=="cleanup-ipc") {steamohos::CleanupSysvIpc(argv[2]);return 0;}
    if(argc!=2)return 2;
    steamohos::Session session;
    if(!steamohos::ReadClientSettings(argv[1],session))return 1;
    printf("geometry=%s observer=%d metadata=%d timeout=%u fullscreen=%d\n",session.desktopGeometry.c_str(),
        session.desktopSession,session.socketMetadata,session.clientTimeout,session.desktopFullscreen);
    for(const auto& value:session.argv)printf("arg=%s\n",value.c_str());
    return 0;
}
''')
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(runner)], check=True)
            config = base / 'client.conf'
            config.write_text('desktopGeometry=1280x800\ndesktopSession=observe\nsocketMetadata=1\n')
            result = subprocess.run([str(runner), str(config)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('geometry=1280x800 observer=1 metadata=1', result.stdout)
            config.write_text('desktopGeometry=1280x800\ndesktopSession=bounds\n')
            result = subprocess.run([str(runner), str(config)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('geometry=1280x800 observer=1 metadata=0', result.stdout)
            config.write_text('desktopGeometry=1280x800\ndesktopFullscreen=1\n')
            result = subprocess.run([str(runner), str(config)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('fullscreen=1', result.stdout)
            # The first action belongs in official Steam argv, after all flags.
            for action, uri in [('library 0', 'steam://open/library'),
                                ('bigPicture 0', 'steam://open/bigpicture'),
                                ('launch 695630', 'steam://rungameid/695630')]:
                config.write_text('startupAction=' + action + '\narg=-cef-disable-gpu\n')
                result = subprocess.run([str(runner), str(config)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines()[-2:], ['arg=-cef-disable-gpu', 'arg=' + uri])
            for value in ['launch 0', 'launch 4294967296', 'launch 695630;touch',
                          'library 12', 'shutdown 0', 'shell 0', 'library 0 extra',
                          'library 0\nstartupAction=downloads 0',
                          'library 0\nlaunchAppId=695630']:
                config.write_text('startupAction=' + value + '\n')
                self.assertEqual(subprocess.run([str(runner), str(config)]).returncode, 1, value)
            config.write_text('launchAppId=695630\nstartupAction=library 0\n')
            self.assertEqual(subprocess.run([str(runner), str(config)]).returncode, 1)
            # Manual login uses no observer, screenshots or synthetic input.
            for timeout in [1000, 60000, 900000, 14400000]:
                config.write_text('timeoutMs=' + str(timeout) + '\n')
                result = subprocess.run([str(runner), str(config)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('observer=0 metadata=0 timeout=' + str(timeout), result.stdout)
            for timeout in ['999', '14400001', '100000000', '-1', '900000x', '']:
                config.write_text('timeoutMs=' + timeout + '\n')
                self.assertEqual(subprocess.run([str(runner), str(config)]).returncode, 1, timeout)
            for value in ['100x100', '8192x8192', '4096x4096', '1280x800;touch /tmp/a',
                          '1280x800x1', '-1280x800', '1280x', '1280x800\nunknown=1']:
                config.write_text('desktopGeometry=' + value + '\n')
                self.assertEqual(subprocess.run([str(runner), str(config)]).returncode, 1, value)
            session = base / 'session'
            metadata = session / 'socket-metadata'
            metadata.mkdir(parents=True, mode=0o700)
            with socket.socket(socket.AF_UNIX) as known, socket.socket(socket.AF_UNIX) as unknown:
                known.bind(str(metadata / '123'))
                unknown.bind(str(metadata / 'unrelated'))
                (metadata / '456').write_text('preserve ordinary files')
                (metadata / '789').symlink_to(metadata / '123')
                subprocess.run([str(runner), 'cleanup', str(session)], check=True)
                self.assertFalse((metadata / '123').exists())
                self.assertTrue((metadata / 'unrelated').exists())
                self.assertTrue((metadata / '456').is_file())
                self.assertTrue((metadata / '789').is_symlink())
            ipc=session/'sysvipc';ipc.mkdir(mode=0o700)
            for name in ['registry-v2','s-1','m-2']:(ipc/name).write_text('generated IPC')
            (ipc/'keep').write_text('unrelated')
            victim=base/'outside';victim.write_text('preserve')
            (ipc/'s-3').symlink_to(victim)
            (ipc/'m-4').mkdir()
            subprocess.run([str(runner),'cleanup-ipc',str(session)],check=True)
            self.assertFalse((ipc/'registry-v2').exists())
            self.assertFalse((ipc/'s-1').exists())
            self.assertFalse((ipc/'m-2').exists())
            self.assertEqual((ipc/'keep').read_text(),'unrelated')
            self.assertTrue((ipc/'s-3').is_symlink())
            self.assertTrue((ipc/'m-4').is_dir())
            self.assertEqual(victim.read_text(),'preserve')
            for name in ['keep','s-3']:(ipc/name).unlink()
            (ipc/'m-4').rmdir()
            subprocess.run([str(runner),'cleanup-ipc',str(session)],check=True)
            self.assertFalse(ipc.exists())
            ipc.symlink_to(base,target_is_directory=True)
            subprocess.run([str(runner),'cleanup-ipc',str(session)],check=True)
            self.assertTrue(ipc.is_symlink())
            self.assertEqual(victim.read_text(),'preserve')


class PrivateSnapshots(unittest.TestCase):
    def test_live_log_bounds_atomic_replace_and_no_follow(self):
        with tempfile.TemporaryDirectory(prefix='steam-private-snapshots-') as temporary:
            base = Path(temporary)
            native = ROOT / 'entry/src/main/cpp/linux_steam'
            source, runner = base / 'snapshot.cpp', base / 'snapshot'
            source.write_text('#include "' + str(native / 'xwayland_probe.cpp') + '"\n' + r'''
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    std::string content = std::string(argv[1])=="oversize" ? std::string(1024*1024+1,'x') : steamohos::ReadHelperLog(argv[2]);
    return steamohos::WritePrivateSnapshot(argv[3],content)?0:1;
}
''')
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(source),
                *[str(native / name) for name in ['native_exec.cpp','native_loader.cpp','native_loader_entry.S']],
                '-pthread','-ldl','-o',str(runner)],check=True,timeout=30)
            log, output = base / 'client.log', base / 'live.private.json'
            content='begin\n'+('x'*70000)+'\nend'
            log.write_text(content)
            def capture(path=log, mode='snapshot'):
                return subprocess.run([str(runner),mode,str(path),str(output)],timeout=3)
            self.assertEqual(capture().returncode,0)
            snapshot=json.loads(output.read_text())
            self.assertEqual(snapshot['readErrno'],0)
            self.assertEqual(snapshot['head'],content[:16384])
            self.assertEqual(snapshot['output'],content[-32768:])
            self.assertEqual(snapshot['fromOffset'],len(content)-32768)
            self.assertEqual(output.stat().st_mode & 0o777,0o600)
            log.write_text('new complete snapshot')
            self.assertEqual(capture().returncode,0)
            self.assertEqual(json.loads(output.read_text())['output'],'new complete snapshot')
            previous=output.read_bytes()
            self.assertEqual(capture(mode='oversize').returncode,1)
            self.assertEqual(output.read_bytes(),previous)
            victim=base/'victim';victim.write_text('keep other file')
            pending=Path(str(output)+'.tmp');pending.symlink_to(victim)
            self.assertEqual(capture().returncode,1)
            self.assertEqual(victim.read_text(),'keep other file')
            self.assertEqual(output.read_bytes(),previous)
            pending.unlink()
            alias=base/'aliased-log';alias.symlink_to(log)
            self.assertEqual(capture(alias).returncode,0)
            self.assertNotEqual(json.loads(output.read_text())['readErrno'],0)
            self.assertNotIn('output',json.loads(output.read_text()))


@unittest.skipUnless(XVFB, 'native host Xvfb is required')
class DesktopSession(unittest.TestCase):
    def test_real_windows_pixels_and_client_exit(self):
        with tempfile.TemporaryDirectory(prefix='gnu-desktop-', dir='/var/tmp') as temporary:
            base = Path(temporary)
            observer, client = base / 'observer', base / 'client'
            (base / 'client.c').write_text(CLIENT)
            for source, target in [(ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture/desktop_session.c', observer),
                                   (base / 'client.c', client)]:
                subprocess.run(['cc', '-O2', *(['-DNDEBUG'] if target == observer else []),
                                '-Wall', '-Wextra', '-Werror', str(source),
                                '-lX11', '-ldl', '-o', str(target)], check=True)
            environment = os.environ.copy()
            # WSLg owns a read-only /tmp/.X11-unix. Use a free abstract Unix
            # address; Xvfb -displayfd otherwise retries filesystem failures.
            number = 200 + os.getpid() % 3000
            for _ in range(32):
                with socket.socket(socket.AF_UNIX) as reservation:
                    try:
                        reservation.bind('\0/tmp/.X11-unix/X' + str(number))
                        break
                    except OSError:
                        number += 1
            else:
                self.fail('No isolated X11 test display is available')
            server = subprocess.Popen([str(XVFB), ':' + str(number), '-screen', '0',
                                       '800x600x24', '-nolisten', 'tcp', '-pn', '-ac'],
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE, env=environment)
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
                environment.update(DISPLAY=':' + str(number), STEAMOHOS_TMP_ROOT=str(base),
                                   STEAMOHOS_DESKTOP_INPUT='1')
                result = subprocess.run([str(observer), str(client)], env=environment,
                                        text=True, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 37, result.stderr)
                self.assertIn('name=Native desktop observer fixture', result.stderr)
                self.assertIn('mapped=1', result.stderr)
                self.assertIn('snapshot width=800 height=600 written=1', result.stderr)
                self.assertIn('diagnostic_keys_sent=8', result.stderr)
                self.assertIn('CLIENT_INPUT text=hmosdeck', result.stdout)
                with (base / 'desktop-root.ppm').open('rb') as image:
                    self.assertEqual(image.readline(), b'P6\n')
                    self.assertEqual(image.readline(), b'800 600\n')
                    self.assertEqual(image.readline(), b'255\n')
                    pixels = image.read()
                self.assertEqual(len(pixels), 800 * 600 * 3)
                offset = (25 * 800 + 25) * 3
                self.assertEqual(pixels[offset:offset + 3], bytes.fromhex('123456'))
                environment.update(STEAMOHOS_WINDOW_BOUNDS='1', HMOSDECK_BOUNDS_FIXTURE='1')
                bounded = subprocess.run([str(observer), str(client)], env=environment,
                                         text=True, capture_output=True, timeout=5)
                self.assertEqual(bounded.returncode, 37, bounded.stderr)
                self.assertIn('CLIENT_BOUNDS constrained=1 override_preserved=1', bounded.stdout)
                self.assertIn('fit_window', bounded.stderr)
                self.assertNotIn('snapshot', bounded.stderr)
                bridge_client = base / 'bridge-client'
                subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                                str(ROOT / 'host_tests/linux_steam_window_bridge_client.c'),
                                '-lX11', '-ldl', '-o', str(bridge_client)], check=True)
                environment.pop('STEAMOHOS_WINDOW_BOUNDS')
                environment.pop('STEAMOHOS_DESKTOP_INPUT')
                environment['STEAMOHOS_WINE_WINDOW_BRIDGE'] = '1'
                (base / 'desktop-root.ppm').unlink()
                bridge = subprocess.run([str(observer), str(bridge_client)], env=environment,
                                        text=True, capture_output=True, timeout=20)
                self.assertEqual(bridge.returncode, 37, bridge.stderr)
                self.assertIn('X11_BRIDGE primary popup fullscreen maximize minimize restore generation PASS', bridge.stdout)
                self.assertIn('X11_CAPTION click drag once menus identity fullscreen PASS', bridge.stdout)
                self.assertIn('X11_DIALOG activation primary-identity host-restore dismissed-focus PASS', bridge.stdout)
                self.assertNotIn('snapshot', bridge.stderr)
                self.assertFalse((base / 'desktop-root.ppm').exists())
                self.assertEqual(json.loads((base / 'wine-window-native.json').read_text())['closed'], 1)
                preferences_client = base / 'preferences-client'
                subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                                str(ROOT / 'host_tests/linux_steam_window_preferences_client.c'),
                                '-lX11', '-o', str(preferences_client)], check=True)
                environment.update(HMOSDECK_SESSION_GENERATION='abcd-1234', STEAMOHOS_CODE_ROOT=str(base))
                for phase in ['initial', 'restored']:
                    environment['HMOSDECK_PLACEMENT_FIXTURE'] = phase
                    result = subprocess.run([str(observer), str(preferences_client)], env=environment,
                                            text=True, capture_output=True, timeout=15)
                    self.assertEqual(result.returncode, 37, result.stderr + result.stdout)
                    self.assertIn('X11_PREFERENCES', result.stdout)
            finally:
                server.terminate()
                server.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
