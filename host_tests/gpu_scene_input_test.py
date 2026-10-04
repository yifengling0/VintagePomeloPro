"""Compile full production compositor, ZC and input files; mock only platform I/O."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--baseline',action='store_true')
p.add_argument('--baseline-ref',default='87b0c5c1ec44a9700faed44217b078ea67de8533');args=p.parse_args()
with tempfile.TemporaryDirectory(prefix='gpu-scene-input-') as directory:
    temp=Path(directory);source=ROOT
    if args.baseline:
        source=temp/'base';source.mkdir()
        archive=subprocess.check_output(['git','archive',args.baseline_ref,'entry/src/main/cpp'],cwd=ROOT)
        subprocess.run(['tar','-x','-C',str(source)],input=archive,check=True)
    cpp=source/'entry/src/main/cpp'
    includes=[ROOT/'host_tests/stubs',cpp,cpp/'compositor',cpp/'compositor/frame',cpp/'compositor/toplevel',cpp/'compositor/input']
    units=['compositor/toplevel/desktop_compositor.cpp','compositor/toplevel/toplevel_manager.cpp',
           'compositor/frame/zc_bridge.cpp','compositor/frame/geometry.cpp','compositor/frame/compositor_utils.cpp',
           'compositor/input/input_resolver.cpp','common/perf_utils.cpp']
    command=['g++','-std=c++17','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-pthread']
    if args.baseline:command+=['-DGPU_FOLLOWUP_BASELINE']
    command += [item for inc in includes for item in ('-I',str(inc))]
    command += [str(ROOT/'host_tests/gpu_scene_input_test.cpp')]+[str(cpp/u) for u in units]
    command += ['-Wl,--gc-sections','-o',str(temp/'test')]
    subprocess.run(command,check=True)
    for mode in range(1 if args.baseline else 5):
        result=subprocess.run([str(temp/'test'),str(mode)],capture_output=True,text=True,timeout=20)
        if args.baseline:
            assert result.returncode!=0 and 'render and real input share GPU-only candidate eligibility' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: real input rejects the GPU-only parent displayed by the renderer')
        else:
            assert result.returncode==0,result.stdout+result.stderr
            print('PASS full production scene/input case',mode)
