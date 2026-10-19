#!/usr/bin/env python3
"""Validate the selected Proton stack and effective overlay sources.
The .yaml lock uses JSON syntax (a YAML subset), requiring no extra parser.
Static and behavior checks are distinct; neither is device qualification.
"""
import argparse, hashlib, io, json, re, subprocess
from zipfile import ZipFile
from pathlib import Path
from dxvk_runtime_contract import load_modern_lock, source_digest, validate_modern_runtime
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--virgl-source',type=Path)
p.add_argument('--wine-source',type=Path)
p.add_argument('--hap',type=Path)
p.add_argument('--native-output-dir',type=Path)
p.add_argument('--behavior',action='store_true')
a=p.parse_args()
lock=json.loads((R/'docs/graphics/graphics-stack.lock.yaml').read_text())
if source_digest(R/'thirdparty/dxvk-modern/src') != load_modern_lock()['sourceSha256']:
    raise SystemExit('graphics-contract: FAIL: Modern DXVK source fingerprint differs from the reviewed binaries')
def check(ok,message):
    if not ok:raise SystemExit('graphics-contract: FAIL: '+message)
def read(name):return (R/name).read_text()
def sha(path):return hashlib.sha256(path.read_bytes().replace(b'\r\n',b'\n')).hexdigest()
def run(*cmd):subprocess.run(cmd,cwd=R,check=True)
check(lock['schema_version']==3,'unsupported lock schema')
for name,pin in lock['submodules'].items():
    row=subprocess.check_output(['git','-c',f'safe.directory={R}','-C',str(R),'ls-files','--stage','--','thirdparty/'+name],text=True)
    check(row.startswith('160000 '+pin+' 0\t'),'gitlink '+name)
for engine,rows in lock['managed_patches'].items():
    for row in rows:check(sha(R/row['path'])==row['sha256'],'overlay content '+row['path'])
    names=[row['path'] for row in rows]
    if engine=='wine-valve':
        actual=re.findall(r'^ensure_wine_patch "\$SCRIPT_DIR/\.\./(patches/wine/[^"\n]+)"',read('scripts/build_wine.sh'),re.M)
        check(actual==names,'effective Wine patch order/registration')
    elif engine=='virglrenderer':
        actual=re.findall(r'\b(\d{4}[-\w]+\.patch)\b',read('scripts/apply_virglrenderer_ohos_patches.sh'))
        check(actual==[Path(n).name for n in names],'VirGL patch order/registration')
    else:
        check(sorted(str(x.relative_to(R)) for x in (R/'patches'/engine).glob('*.patch'))==names,engine+' overlay inventory')
protocol=read('entry/src/main/cpp/graphics/virgl_ipc_protocol.h')
for field,key in [('kProtocolVersion','whip_version'),('kHostConfigStringCount','host_config_strings')]:
    check(int(re.search(field+r'\s*=\s*(\d+)',protocol)[1])==lock['protocol'][key],field)
make=read('Makefile')
check('WINE_SRC ?= $(ROOT)/thirdparty/wine-valve' in make,'default Wine source')
check('WINE_ARCH ?= $(if $(filter arm64-v8a,$(NATIVE_ARCH)),aarch64,x86_64)' in make,'native Wine architecture')
check('BUILD_GUEST_GFX ?= 1' in make,'guest graphics enabled')
profile=read('entry/src/main/cpp/graphics/graphics_profile.cpp')
check('true,  // batchMappedFlush' in profile,'DXVK mapped-flush capability')
env=read('entry/src/main/cpp/wine/wine_env.cpp')
check('VN_WINEHUA_STRONG_RING_BARRIER' in env,'Venus ring memory ordering')
native=read('entry/src/main/cpp/bridge/napi_init.cpp')
check('TextInput::GetInstance()->IsEnabled()' in native,'IME enabled API must query state')
check('StopRegisteredWineSession(id)' in native,'session stop must use incarnation')
check('RemoveProcess(pid);' not in native[native.index('static napi_value KillProcess('):native.index('// -- 模块注册 --')],'kill request must not fabricate process exit')
present=read('entry/src/main/cpp/graphics/virgl_surface_presenter.cpp')
check('failureBackoff_.PendingDeadline(nowNs)' in present and 'failureBackoff_.Fail(' in present,'GL present failure pacing')
direct=read('entry/src/main/cpp/graphics/native_window_vk_target.cpp')
for name in ['vkAcquireImageOHOS','vkQueueSignalReleaseImageOHOS','OH_NativeWindow_NativeWindowFlushBuffer']:
    check(name in direct,'Direct ownership '+name)
for name in ['vkQueuePresentKHR','vkAcquireNextImageKHR','glReadPixels']:check(name not in direct,'Direct CPU/WSI bypass '+name)
model=read('entry/src/main/ets/model/AppModels.ets')
check('WINE_MF_PREFER_MPEG4_HANDLER=1' not in model,'redundant MPEG4 setting emission')
for engine,src in [('wine-valve',a.wine_source),('virglrenderer',a.virgl_source)]:
    if src:
        for name,expected in lock['effective_files'][engine].items():
            check((src/name).is_file() and sha(src/name)==expected,'effective '+engine+'/'+name)
if not a.hap:
    with ZipFile(R/'entry/src/main/resources/rawfile/wine-data.zip') as wine:
        validate_modern_runtime(wine, lock)
if a.hap:
    with ZipFile(a.hap) as hap:
        manifest=json.loads(hap.read('resources/rawfile/wine-runtime-manifest.json'))
        record=manifest.get('graphicsMigration',{})
        check(record.get('lockSha256')==hashlib.sha256((R/'docs/graphics/graphics-stack.lock.yaml').read_bytes()).hexdigest(),'packaged migration lock')
        for name,row in record.get('virgl',{}).items():
            check(hashlib.sha256(hap.read('libs/arm64-v8a/'+name)).hexdigest()==row['sha256'],'packaged VirGL '+name)
        check(set(record.get('virgl',{}))=={'libvirglrenderer.so.1','libwinehua_vtest_server.so'},'packaged VirGL inventory')
        payload=hap.read('resources/rawfile/wine-data.zip')
        check(hashlib.sha256(payload).hexdigest()==manifest['payloadSha256'],'packaged payload')
        with ZipFile(io.BytesIO(payload)) as wine:
            validate_modern_runtime(wine, lock)
            check(set(record.get('mfplat',{}))=={'bin/aarch64-windows/mfplat.dll','bin/i386-windows/mfplat.dll'},'packaged MF inventory')
            for name,row in record['mfplat'].items():
                data=wine.read(name)
                check(hashlib.sha256(data).hexdigest()==row['sha256'],'packaged MF '+name)
                check(b'WINEHUA_MF_FALLBACK_NONSHARED' in data,'packaged MF opt-in code '+name)
        app=json.loads(hap.read('module.json'))['app']
        check(app['bundleName']=='com.vintage.pomelopro' and not app['debug'],'release candidate identity')
        check(app['minAPIVersion']==60100023 and app['targetAPIVersion']==60100023,'candidate API 23')
        version=re.search(r'"versionName"\s*:\s*"([^"\n]+)"',read('AppScope/app.json5'))[1]
        check(app['versionName']==version,'candidate version')
        check(not any(n.startswith('META-INF/') for n in hap.namelist()),'candidate must be unsigned')
        if a.native_output_dir:
            for name in ['libentry.so','libvirgl_child.so','libwine_child.so']:
                check(hap.read('libs/arm64-v8a/'+name)==(a.native_output_dir/name).read_bytes(),'final compiled Native '+name)
    print('graphics-contract: final HAP module identities, actual MF code, runtime/lock hashes and native outputs PASS')
print('graphics-contract: pins, ordered overlays, protocol, routes, lifetime and effective-source checks PASS',flush=True)
if a.behavior:
    check(a.wine_source is not None and a.virgl_source is not None,'behavior mode requires effective Wine and VirGL sources')
    run('python3','host_tests/gpu_scene_input_test.py')
    run('python3','host_tests/virgl_srgb_policy_test.py','--source',str(a.virgl_source))
    run('python3','host_tests/virgl_present_context_test.py','--source',str(a.virgl_source))
    run('python3','host_tests/mf_nonshared_retry_test.py','--source',str(a.wine_source))
