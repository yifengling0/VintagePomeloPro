"""Regression for the real config.h / Makefile combination that broke PAL4."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
script = root / 'scripts/wine_graphics_capabilities.py'
spec = importlib.util.spec_from_file_location('capabilities', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='wine-egl-regression-') as temporary:
    build = Path(temporary)
    (build / 'include').mkdir()
    config = build / 'include/config.h'
    makefile = build / 'Makefile'
    config.write_text('/* #undef HAVE_LIBWAYLAND_EGL */\n')
    makefile.write_text('WAYLAND_CLIENT_LIBS = -lwayland-client\nWAYLAND_EGL_LIBS =\nXKBCOMMON_LIBS = -lxkbcommon\n')
    command = ['python3', str(script), '--build', str(build)]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and 'HAVE_LIBWAYLAND_EGL' in result.stderr
    config.write_text('#define HAVE_LIBWAYLAND_EGL 1\n')
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode != 0 and 'WAYLAND_EGL_LIBS' in result.stderr
    makefile.write_text('WAYLAND_EGL_LIBS = -L/target/sysroot/lib -lwayland-egl\n')
    subprocess.run(command, check=True)
    # A successful ELF inspection of an unrelated native object must not pass.
    result = subprocess.run(command + ['--driver', '/bin/true'], capture_output=True, text=True)
    assert result.returncode != 0 and 'wl_egl_window_create' in result.stderr
print('Wayland EGL omission, empty link flags, valid configuration and stub rejection passed')
