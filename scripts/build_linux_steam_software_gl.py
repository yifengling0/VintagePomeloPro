#!/usr/bin/env python3
"""Build GNU ARM64 Mesa softpipe/Xlib for the opt-in Linux Steam diagnostic.

Uses the existing Mesa submodule. Produces genuine software OpenGL/GLX over
Xlib/Xwayland, without a DRM device, a translated CPU, or OHOS libc loading GNU.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc",required=True,type=Path)
    parser.add_argument("--meson",required=True,type=Path)
    parser.add_argument("--xwayland-build",required=True,type=Path)
    parser.add_argument("--artifacts",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--jobs",type=int,default=12)
    args=parser.parse_args()
    if args.output.exists() or not 1<=args.jobs<=32:parser.error("Use a new output and 1-32 jobs")
    cc=args.cc.resolve(strict=True);meson=args.meson.resolve(strict=True)
    target=subprocess.check_output([str(cc),"-dumpmachine"],text=True).strip()
    if not target.startswith("aarch64") or "linux-gnu" not in target:parser.error("GNU ARM64 compiler required")
    original_source=ROOT/"thirdparty/mesa"
    source=original_source
    commit=subprocess.check_output(["git","rev-parse","HEAD"],cwd=source,text=True).strip()
    subprocess.run(["git","diff","--quiet","HEAD","--"],cwd=source,check=True)
    output=args.output.resolve();output.mkdir(parents=True)
    # Keep the Wine Mesa submodule untouched; apply the Xlib adapter privately.
    archive=output/"mesa-source.tar"
    subprocess.run(["git","archive","--format=tar","--output="+str(archive),commit],cwd=original_source,check=True)
    source=output/"mesa-source";source.mkdir()
    subprocess.run(["tar","-xf",str(archive),"-C",str(source),"--no-same-owner"],check=True)
    archive.unlink()
    # Share the real VirGL guest capabilities with Wine. Xlib alone otherwise
    # stops at GL 2.1 on GLES hosts without native sRGB write control. These
    # patches implement the missing semantics; never override GL versions.
    patches=[ROOT/"patches/mesa"/name for name in [
        "0006-virgl-vtest-serialize-socket-transactions.patch",
        "0007-virgl-gpu-srgb-surface-storage.patch",
        "0001-xlib-present-with-a-valid-pipe-context.patch",
        "0002-xlib-prefer-compatible-screen-visual.patch",
        "0003-xlib-fbconfig-and-compressed-formats.patch"]]
    for patch in patches:
        with patch.open("rb") as changes:
            subprocess.run(["patch","-p1","--forward","--batch"],cwd=source,stdin=changes,check=True)
    old=args.xwayland_build.resolve()/"deps-root"
    deps=output/"deps-root";shutil.copytree(old,deps,symlinks=True)
    lock_path=args.artifacts/"debian-software-gl.lock.json"
    lock=json.loads(lock_path.read_text())
    for record in lock.values():
        package=args.artifacts/"software-gl-debs"/Path(record["Filename"]).name
        if record["Architecture"]!="arm64" or sha(package)!=record["SHA256"]:parser.error("ARM64 package checksum mismatch")
        subprocess.run(["dpkg-deb","-x",str(package),str(deps)],check=True)
    for path in deps.rglob("*"):
        if path.is_symlink():
            link=os.readlink(path)
            if link.startswith("/"):
                path.unlink();path.symlink_to(os.path.relpath(deps/link.lstrip("/"),path.parent))
    prefix=deps/"usr";lib=prefix/"lib/aarch64-linux-gnu"
    for pc in prefix.rglob("*.pc"):
        if not pc.is_symlink():pc.write_text(pc.read_text().replace(str(old),str(deps)).replace("prefix=/usr","prefix="+str(prefix)))
    cross=output/"cross.ini"
    binaries={"c":cc,"cpp":cc.with_name(cc.name.replace("gcc","g++")),"ar":cc.with_name(cc.name.replace("gcc","ar")),"strip":cc.with_name(cc.name.replace("gcc","strip")),"pkg-config":"/usr/bin/pkg-config"}
    cross.write_text("[binaries]\n"+"".join(k+" = '"+str(v)+"'\n" for k,v in binaries.items())+
        "[host_machine]\nsystem='linux'\ncpu_family='aarch64'\ncpu='aarch64'\nendian='little'\n"+
        "[properties]\nneeds_exe_wrapper=true\npkg_config_libdir=['"+str(lib/"pkgconfig")+"','"+str(prefix/"share/pkgconfig")+"']\n"+
        "[built-in options]\nc_args=['-I"+str(prefix/"include")+"']\ncpp_args=['-I"+str(prefix/"include")+"']\n"+
        "c_link_args=['-Wl,--build-id=sha1','-L"+str(lib)+"','-Wl,-rpath-link,"+str(lib)+":"+str(deps/"lib/aarch64-linux-gnu")+"']\n"+
        "cpp_link_args=['-Wl,--build-id=sha1','-L"+str(lib)+"','-Wl,-rpath-link,"+str(lib)+":"+str(deps/"lib/aarch64-linux-gnu")+"']\n")
    env=os.environ.copy();env["PATH"]=str(meson.parent)+":"+str(cc.parent)+":"+env["PATH"]
    env["PKG_CONFIG_PATH"]="";env["PKG_CONFIG_LIBDIR"]=str(lib/"pkgconfig")+":"+str(prefix/"share/pkgconfig")
    options=["--buildtype=release","--wrap-mode=nofallback","-Dplatforms=x11","-Dgallium-drivers=softpipe,virgl",
        "-Dglx=xlib","-Dxlib-lease=disabled","-Dglvnd=disabled","-Dshared-glapi=disabled","-Degl=disabled","-Dgbm=disabled",
        "-Dllvm=disabled","-Dvulkan-drivers=[]","-Dgles1=disabled","-Dgles2=disabled",
        "-Dgallium-vdpau=disabled","-Dgallium-va=disabled","-Dlibunwind=disabled","-Dzstd=disabled",
        "-Dvalgrind=disabled","-Dlmsensors=disabled","-Dbuild-tests=false","-Dtools=[]"]
    build=output/"build"
    commands=[("configure",[meson,"setup",build,source,"--cross-file="+str(cross),*options]),
              ("build",["ninja","-C",build,"-j"+str(args.jobs)])]
    for name,command in commands:
        with (output/(name+".log")).open("w") as log:
            result=subprocess.run([str(x) for x in command],env=env,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:
            print((output/(name+".log")).read_text()[-12000:]);raise SystemExit(result.returncode)
    library=build/"src/gallium/targets/libgl-xlib/libGL.so.1.5.0"
    destination=output/"libGL.so.1";shutil.copyfile(library,destination)
    build_id=re.search(r"Build ID: ([0-9a-f]{40})",subprocess.check_output(["readelf","-n",str(destination)],text=True))
    if not build_id:raise RuntimeError("virgl shader cache requires a real SHA-1 ELF Build ID")
    data=destination.read_bytes()
    if data[:4]!=b"\x7fELF" or int.from_bytes(data[18:20],"little")!=183:raise RuntimeError("Expected ARM64 ELF")
    fixture=ROOT/"entry/src/main/cpp/linux_steam/gnu_fixture/glx.cpp"
    probe=output/"steamohos_gnu_glx"
    subprocess.run([str(binaries["cpp"]),"-std=c++17","-O2","-Wall","-Wextra","-Werror",
        "-I"+str(source/"include"),"-I"+str(prefix/"include"),str(fixture),"-L"+str(output),
        "-l:libGL.so.1","-L"+str(lib),"-lX11","-Wl,-rpath-link,"+str(lib)+":"+str(deps/"lib/aarch64-linux-gnu"),
        "-Wl,-z,relro,-z,now","-o",str(probe)],check=True)
    manifest={"schemaVersion":1,"executionModel":"native_aarch64","renderers":["softpipe", "virpipe"],"presentation":"Mesa Xlib GLX over Xwayland",
        "elfBuildId":build_id.group(1),"mesaCommit":commit,"mesaVersion":(source/"VERSION").read_text().strip(),"compiler":str(cc),
        "patches":{str(patch.relative_to(ROOT)):sha(patch) for patch in patches},"options":options,"packages":lock,"sources":{str(fixture.relative_to(ROOT)):sha(fixture),str(Path(__file__).relative_to(ROOT)):sha(Path(__file__))},"files":{"libGL.so.1":{"sha256":sha(destination),"bytes":destination.stat().st_size},"steamohos_gnu_glx":{"sha256":sha(probe),"bytes":probe.stat().st_size}}}
    (output/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    print(json.dumps(manifest))
if __name__=="__main__":main()
