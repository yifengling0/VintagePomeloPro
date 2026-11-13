"""Exercise the opt-in host probe against a real Wayland protocol fixture.

This is a Linux reference for the client, not a product compositor/device test.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinuxSteamHost(unittest.TestCase):
    def test_imported_registry_wire_contract_and_fd_ownership(self):
        host = ROOT / 'entry/src/main/cpp/linux_steam/host_v1'
        with tempfile.TemporaryDirectory(prefix='vpp-steamohos-registry-') as directory:
            runner = Path(directory) / 'runner'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(host), str(ROOT / 'host_tests/linux_steam_registry_test.cpp'),
                            str(host / 'wayland_probe.cpp'), '-pthread', '-o', str(runner)],
                           check=True, timeout=30)
            run = subprocess.run([str(runner)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout, end='')

    def test_shm_lifecycle_keyboard_and_rejected_incomplete_hosts(self):
        with tempfile.TemporaryDirectory(prefix='vpp-steamohos-host-') as directory:
            protocol = Path(directory) / 'xdg.o'
            includes = ['-I' + str(ROOT / 'entry/src/main/cpp'),
                        '-I' + str(ROOT / 'entry/src/main/cpp/protocols')]
            subprocess.run(['cc', *includes, '-c',
                            str(ROOT / 'entry/src/main/cpp/protocols/xdg-shell-protocol.c'),
                            '-o', str(protocol)], check=True, timeout=30)
            runner = Path(directory) / 'runner'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            *includes, str(ROOT / 'host_tests/linux_steam_host_test.cpp'),
                            str(ROOT / 'entry/src/main/cpp/linux_steam/surface_probe.cpp'),
                            str(protocol), '-pthread', '-lwayland-client', '-lwayland-server',
                            '-o', str(runner)], check=True, timeout=30)
            run = subprocess.run([str(runner)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.count('pass=1'), 6, run.stdout)
            print(run.stdout, end='')


if __name__ == '__main__':
    unittest.main()
