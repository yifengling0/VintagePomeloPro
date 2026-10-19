"""Compile production CPU compositor and final GPU scene; mock platform I/O."""
from pathlib import Path
import subprocess, tempfile, sys
R=Path(__file__).resolve().parents[1]
C=R/'entry/src/main/cpp'
with tempfile.TemporaryDirectory(prefix='compositor-migration-') as d:
    out=Path(d)
    header=out/'platform.h'
    header.write_text('#include <sys/types.h>\nstruct wl_client;\nvoid wl_client_get_credentials(wl_client*, pid_t*, uid_t*, gid_t*);\n')
    includes=[R/'host_tests/stubs',C,C/'compositor',C/'compositor/frame',C/'compositor/toplevel',C/'compositor/input']
    units=['compositor/toplevel/desktop_compositor.cpp','compositor/toplevel/toplevel_manager.cpp',
           'compositor/toplevel/popup_manager.cpp','compositor/frame/zc_bridge.cpp','compositor/frame/geometry.cpp',
           'compositor/frame/compositor_utils.cpp','compositor/frame/frame_pipeline.cpp','compositor/frame/frame_composer.cpp',
           'compositor/frame/compositor_blit.cpp','compositor/input/input_resolver.cpp','common/perf_utils.cpp']
    cmd=['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable','-Wno-unused-result',
         '-ffunction-sections','-fdata-sections','-pthread','-include',str(header)]
    if '--asan' in sys.argv:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-no-pie']
    cmd += [a for inc in includes for a in ['-I',str(inc)]]
    cmd += [str(R/'host_tests/compositor_migration_test.cpp')]+[str(C/u) for u in units]+['-Wl,--gc-sections','-o',str(out/'test')]
    subprocess.run(cmd,check=True)
    subprocess.run([str(out/'test')],check=True,timeout=60)
