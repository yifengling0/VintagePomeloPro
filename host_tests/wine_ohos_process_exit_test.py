"""Replay Wine's production exit wrapper against the appspawn exit guard."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = 'cd547f7a0e'
PATH = 'dlls/ntdll/unix/server.c'


def wrapper(text):
    start = text.index('void process_exit_wrapper( int status )')
    end = text.index('\n}', start) + 2
    return text[start:end]


class OhosProcessExit(unittest.TestCase):
    def test_real_wrapper_does_not_enter_appspawn_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            path = tree / PATH
            path.parent.mkdir(parents=True)
            original = subprocess.check_output(['git', '-C', str(ROOT/'thirdparty/wine-valve'),
                                               'show', f'{PIN}:{PATH}'], text=True)
            path.write_text(original)
            with (ROOT/'patches/wine/0046-ntdll-ohos-process-exit.patch').open() as f:
                subprocess.run(['patch', '--batch', '--force', '-s', '-p1'],
                               cwd=tree, stdin=f, check=True)
            fixed = wrapper(path.read_text())
            # The real close runs: a socket must be released before termination.
            prelude = '#include <stdlib.h>\n#include <stdio.h>\n#include <unistd.h>\n#include <fcntl.h>\n#include <errno.h>\nstatic int fd_socket;\nstatic void checked_exit(int status) { if(fcntl(fd_socket,F_GETFD)!=-1 || errno!=EBADF) _Exit(93); _Exit(status); }\nstatic void appspawn_exit(int status) { (void)status; _Exit(86); }\n#define exit appspawn_exit\n#define _exit checked_exit\n'
            main = '\nint main(int argc,char **argv) { if(argc!=2)return 90; fd_socket=open("/dev/null",O_RDONLY); if(fd_socket<0)return 91; process_exit_wrapper(atoi(argv[1])); return 92; }\n'
            for name, body, ohos in [('old', wrapper(original), True),
                                      ('ohos', fixed, True), ('other', fixed, False)]:
                source = tree/(name+'.c')
                source.write_text(prelude+body+main)
                binary = tree/name
                subprocess.run([os.environ.get('CC','cc'), '-O2', *(['-D__OHOS__'] if ohos else []),
                                str(source), '-o', str(binary)], check=True)
                for status in (0, 7, 255):
                    result = subprocess.run([str(binary), str(status)])
                    self.assertEqual(result.returncode, status if name=='ohos' else 86)
                print(f'{name}: statuses 0/7/255 verified')


if __name__ == '__main__':
    unittest.main()
