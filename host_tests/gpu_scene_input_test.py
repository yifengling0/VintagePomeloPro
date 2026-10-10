"""Compile full production compositor, ZC and input files; mock only platform I/O."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--baseline',action='store_true')
p.add_argument('--baseline-ref',default='87b0c5c1ec44a9700faed44217b078ea67de8533')
p.add_argument('--focus-baseline',action='store_true',
               help='Require the keyboard focus regression to fail against the prior production implementation')
p.add_argument('--focus-baseline-ref',default='0a877ea3724cf02e248d90c413aac92afbb38fd0')
p.add_argument('--resize-baseline',action='store_true',help='Reproduce stale SHM dimensions with prior production ZC code')
p.add_argument('--resize-baseline-ref',default='f47090663537b1e70ad1ebe2f76bd2c34cfbb6af')
p.add_argument('--viewport-baseline',action='store_true',help='Reproduce padded SHM menu compression with prior compositor')
p.add_argument('--input-region-baseline',action='store_true',help='Reproduce Direct GPU child intercepting pointer input')
args=p.parse_args()
assert not (args.baseline and args.focus_baseline)
with tempfile.TemporaryDirectory(prefix='gpu-scene-input-') as directory:
    temp=Path(directory);source=ROOT
    if args.baseline:
        source=temp/'base';source.mkdir()
        archive=subprocess.check_output(['git','archive',args.baseline_ref,'entry/src/main/cpp'],cwd=ROOT)
        subprocess.run(['tar','-x','-C',str(source)],input=archive,check=True)
    cpp=source/'entry/src/main/cpp'
    includes=[ROOT/'host_tests/focus_stubs',ROOT/'host_tests/stubs',cpp,cpp/'compositor',
              cpp/'compositor/frame',cpp/'compositor/toplevel',cpp/'compositor/input']
    units=['compositor/toplevel/desktop_compositor.cpp','compositor/toplevel/toplevel_manager.cpp',
           'compositor/frame/zc_bridge.cpp','compositor/frame/geometry.cpp','compositor/frame/compositor_utils.cpp',
           'compositor/input/input_resolver.cpp','common/perf_utils.cpp']
    if not args.baseline:
        units+=['compositor/input/input_injector.cpp','compositor/input/input_state_tracker.cpp',
                'compositor/input/input_queue.cpp','input/input_manager.cpp']
    command=['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
             '-ffunction-sections','-fdata-sections','-pthread']
    if args.baseline:command+=['-DGPU_FOLLOWUP_BASELINE']
    command += [item for inc in includes for item in ('-I',str(inc))]
    sources=[str(cpp/u) for u in units]
    if args.input_region_baseline:
        unit='compositor/input/input_resolver.cpp'
        resolver=temp/'baseline-input-resolver.cpp'
        resolver.write_bytes(subprocess.check_output(['git','show',args.resize_baseline_ref+':entry/src/main/cpp/'+unit],cwd=ROOT))
        sources[sources.index(str(cpp/unit))]=str(resolver)
    if args.viewport_baseline:
        unit='compositor/toplevel/desktop_compositor.cpp'
        baseline=temp/'baseline-desktop-compositor.cpp'
        baseline.write_bytes(subprocess.check_output(['git','show',args.resize_baseline_ref+':entry/src/main/cpp/'+unit],cwd=ROOT))
        sources[sources.index(str(cpp/unit))]=str(baseline)
    if args.resize_baseline:
        unit='compositor/frame/zc_bridge.cpp'
        bridge=temp/'baseline-zc-bridge.cpp'
        bridge.write_bytes(subprocess.check_output(['git','show',args.resize_baseline_ref+':entry/src/main/cpp/'+unit],cwd=ROOT))
        sources[sources.index(str(cpp/unit))]=str(bridge)
    if args.focus_baseline:
        unit='compositor/input/input_injector.cpp'
        injector=temp/'baseline-injector.cpp'
        injector.write_bytes(subprocess.check_output(
            ['git','show',args.focus_baseline_ref+':entry/src/main/cpp/'+unit],cwd=ROOT))
        sources[sources.index(str(cpp/unit))]=str(injector)
        unit='input/input_manager.cpp'
        manager=temp/'baseline-manager.cpp'
        manager.write_bytes(subprocess.check_output(
            ['git','show',args.focus_baseline_ref+':entry/src/main/cpp/'+unit],cwd=ROOT))
        sources[sources.index(str(cpp/unit))]=str(manager)
        command+=['-I',str(cpp/'input')]
    command += [str(ROOT/'host_tests/gpu_scene_input_test.cpp')]+sources
    command += ['-Wl,--gc-sections','-o',str(temp/'test')]
    subprocess.run(command,check=True)
    modes=[14] if args.input_region_baseline else ([12] if args.viewport_baseline else ([9] if args.resize_baseline else ([6] if args.focus_baseline else range(1 if args.baseline else 15))))
    for mode in modes:
        result=subprocess.run([str(temp/'test'),str(mode)],capture_output=True,text=True,timeout=20)
        if args.input_region_baseline:
            assert result.returncode!=0 and 'Direct input-transparent drawable must delegate' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: Direct GPU-only child intercepts parent pointer events')
        elif args.viewport_baseline:
            assert result.returncode!=0 and 'padded menu buffer must be cropped before scaling' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: padded SHM menu buffer is compressed into the logical destination')
        elif args.resize_baseline:
            assert result.returncode!=0 and 'info.width==480&&info.height==320' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: consumed native resize is hidden by stale SHM dimensions')
        elif args.focus_baseline:
            assert result.returncode!=0 and 'GetKeyboardFocusedToplevel()==4' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: prior production click pre-sets the new focus before its queued leave')
        elif args.baseline:
            assert result.returncode!=0 and 'render and real input share GPU-only candidate eligibility' in result.stderr,result.stdout+result.stderr
            print('BASELINE RED: real input rejects the GPU-only parent displayed by the renderer')
        else:
            assert result.returncode==0,result.stdout+result.stderr
            print('PASS full production scene/input case',mode)
