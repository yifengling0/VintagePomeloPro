"""Exercise the production resource reference against real libwayland lifetime events."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='vp-subsurface-lifetime-') as directory:
    binary = Path(directory) / 'test'
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-server'], text=True).split()
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I', str(ROOT / 'entry/src/main/cpp'), str(ROOT / 'host_tests/subsurface_resource_test.cpp'),
        *flags, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
