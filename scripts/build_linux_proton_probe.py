#!/usr/bin/env python3
"""Build the opt-in GNU Proton probe shim; never modifies the product runtime."""
from pathlib import Path
import argparse, hashlib, json, subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-build',type=Path,required=True,help='Existing verified GNU runtime build with manifest and objects')
parser.add_argument('--output',type=Path,required=True,help='Reusable experiment directory outside the runtime')
parser.add_argument('--cc',type=Path,required=True,help='GNU AArch64 gcc')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
base=args.base_build.resolve(strict=True)
task=args.output.resolve()
if task.is_relative_to(base) or task==root:
    parser.error('Keep the experiment outside the original GNU runtime/build')
task.mkdir(parents=True,exist_ok=True)
cc=args.cc.resolve(strict=True)
cxx=cc.with_name(cc.name.replace('gcc','g++'))
assert subprocess.check_output([str(cc),'-dumpmachine'],text=True).strip().startswith('aarch64')
fixture = root/'entry/src/main/cpp/linux_steam/gnu_fixture'
manifest = json.loads((base/'runtime/manifest.json').read_text())
for name in ['posix_compat.c','sysvipc_compat.c','filesystem_compat.c','popen_compat.c','socket_metadata.c',
             'exec_compat.cpp','syscall_compat.S']:
    source = fixture/name
    assert manifest['sources'][str(source.relative_to(root))] == hashlib.sha256(source.read_bytes()).hexdigest()

for name in ['native_loader.cpp','native_loader_entry.S','native_loader.h']:
    source=fixture.parent/name
    assert manifest['sources'][str(source.relative_to(root))]==hashlib.sha256(source.read_bytes()).hexdigest()

original = (fixture/'exec_compat.cpp').read_text()
assert original.count('code+"/libsteamohos_posix.so"') == 1
variant = original.replace('code+"/libsteamohos_posix.so"','code+"/libproton_probe.so"')
variant = '#include <poll.h>\n' + variant
# Wine starts explorer/services with exec. Their ELF dependency search must
# retain the private Proton closure, just like the first Wine process.
search = 'code + \':\' + code + "/steamrtarm64:" + code + "/steamrtarm64/libs:" + code + "/steamrtarm64/video"'
assert variant.count(search) == 1
variant = variant.replace(search, 'code + "/graphics:" + code + ":" + code + "/package/files/lib/wine/aarch64-unix:" + code + "/package/files/lib:" + code + "/package/files/lib/aarch64-linux-gnu"')
# GNU Wine uses its separately built SDL2 ControllerHub adapter, never Steam SDL3.
variant = variant.replace('    const char* gamepad=getenv("STEAMOHOS_GAMEPAD_SOCKET");\n', '')
variant = variant.replace('(gamepad && *gamepad ? ":"+code+"/libsteamohos_gamepad.so" : "")', '""')

preloader = '    const std::string target = Executable(filename, code);'
assert variant.count(preloader) == 1
variant = variant.replace(preloader, preloader + r'''
    // An optional static-PIE preloader would fall through rtld to forbidden
    // kernel execve. Report it unsupported so Wine executes its real loader.
    if(target==code+"/package/files/lib/wine/aarch64-unix/wine-preloader") return Fail(ENOEXEC);
''', 1)
needle = '    int result=next(pid,path,actions,attr,argv,env);'
assert variant.count(needle) == 1
variant = variant.replace(needle, r'''
    // Wine 11 uses posix_spawn for wineserver. Preserve the actual child and
    // errno handshake for this supported no-actions subset. Other spawn
    // contracts continue to the original libc implementation unchanged.
    const char* codeRoot=getenv("STEAMOHOS_CODE_ROOT");
    if(!search&&!actions&&!flags&&codeRoot&&
       !strcmp(codeRoot,"/data/storage/el2/base/files/steamohos-native/proton-probe")&&
       !Executable(path,codeRoot).empty()&&Threads()==1) {
        int errors[2];if(pipe2(errors,O_CLOEXEC))return errno;
        pid_t child=fork();
        if(child<0){int error=errno;close(errors[0]);close(errors[1]);return error;}
        if(!child){
            close(errors[0]);Handoff(path,argv,env);int error=errno;
            (void)write(errors[1],&error,sizeof(error));_exit(127);
        }
        close(errors[1]);
        pollfd event{errors[0],POLLIN,0};
        int ready;do{ready=poll(&event,1,5000);}while(ready<0&&errno==EINTR);
        int error=ETIMEDOUT;ssize_t count=-1;
        if(ready>0){do{count=read(errors[0],&error,sizeof(error));}while(count<0&&errno==EINTR);}
        close(errors[0]);
        if(ready>0&&count==0){if(pid)*pid=child;fprintf(stderr,"HMOSDECK_PROTON_SPAWN child=%d success=1\n",child);return 0;}
        kill(child,SIGKILL);while(waitpid(child,nullptr,0)<0&&errno==EINTR){}
        return count==sizeof(error)?error:(ready==0?ETIMEDOUT:EIO);
    }
''' + needle)
guard = '    steamohos::NativeLoaderImage image;'
assert variant.count(guard) == 1
variant = variant.replace(guard, r'''
    // A display-init failure must not recursively launch an explorer tree.
    // Only this explicit probe uses a single desktop slot; product Wine is
    // untouched. The test controller removes the slot before its next run.
    bool desktop=false;
    if(arguments)for(size_t n=1;arguments[n]&&n<120;++n) {
        const char* value=arguments[n];size_t size=strlen(value);
        if(size>=12&&!strcasecmp(value+size-12,"explorer.exe"))desktop=true;
    }
    if(desktop) {
        const char* slot="/data/storage/el2/base/files/steamohos-native/proton-probe/desktop-probe.once";
        int desktopFd=open(slot,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(desktopFd<0){fprintf(stderr,"HMOSDECK_PROTON_DESKTOP_RETRY_BLOCKED errno=%d\n",errno);return Fail(errno==EEXIST?ELOOP:errno);}
        close(desktopFd);
    }
''' + guard, 1)
variant += r'''
// Wayland 1.23 only adds a diagnostic label to event-queue creation.
// Retain real Wayland queue allocation/dispatch semantics on the existing
// 1.22 runtime; prefer the actual named API when a newer client is loaded.
struct wl_display;
struct wl_event_queue;
extern "C" wl_event_queue* wl_display_create_queue_with_name(wl_display* display,const char* name) {
    using Named=wl_event_queue*(*)(wl_display*,const char*);
    using Plain=wl_event_queue*(*)(wl_display*);
    static void* library=dlopen("libwayland-client.so.0",RTLD_NOW|RTLD_NOLOAD);
    if(!library){errno=ENOSYS;return nullptr;}
    static auto named=reinterpret_cast<Named>(dlsym(library,"wl_display_create_queue_with_name"));
    if(named && named!=&wl_display_create_queue_with_name)return named(display,name);
    static auto plain=reinterpret_cast<Plain>(dlsym(library,"wl_display_create_queue"));
    if(!plain){errno=ENOSYS;return nullptr;}
    auto* queue=plain(display);
    fprintf(stderr,"HMOSDECK_PROTON_WAYLAND_QUEUE real_queue=%d named_api=0\n",queue!=nullptr);
    return queue;
}
'''
variant += r'''
// Wine's upstream event loop explicitly falls back to epoll_wait on ENOSYS.
// glibc issues epoll_pwait2 with an internal syscall, bypassing syscall@PLT.
extern "C" int epoll_pwait2(int, struct epoll_event*, int,
                           const struct timespec*, const sigset_t*) {
    errno=ENOSYS; return -1;
}
'''
# Wine reconstructs Unix envp when it creates a Windows child. Retain the
# private GNU transport/drive capabilities at that handoff, just as the product
# adapter retains its audio and SysV IPC namespace. Do not depend on arbitrary
# Unix environment variables surviving Wine's Windows environment conversion.
variant = variant.replace('    const char* software=getenv("STEAMOHOS_SOFTWARE_GL");', r'''
    for (const char* key : {"STEAMOHOS_PROTON_GAME_ROOT", "STEAMOHOS_PROTON_GAME_DRIVE",
            "STEAMOHOS_PROTON_DRIVE_ROOT", "STEAMOHOS_PROTON_NATIVE_EGL",
            "STEAMOHOS_PROTON_NATIVE_VULKAN", "STEAMOHOS_PROTON_VULKAN_MAP32",
            "WINEHUA_VTEST_PRESENT", "LIBGL_DRIVERS_PATH", "VN_DEBUG", "VN_PERF",
            "VN_WINEHUA_STRONG_RING_BARRIER", "VN_WINEHUA_REMOTE_MEMORY_SYNC",
            "VN_WINEHUA_PERSISTENT_MAP_SYNC", "VN_WINEHUA_DIRECT_FENCE_WAIT",
            "STEAMOHOS_PROTON_LOCALE", "LANG", "LC_ALL", "LC_CTYPE", "LC_MESSAGES", "LOCPATH", "FONTCONFIG_FILE", "FONTCONFIG_PATH",
            "WINEPATH", "WINEDLLOVERRIDES", "WINE_USE_EGL", "WINE_DISABLE_FULLSCREEN_HACK", "WINE_LARGE_ADDRESS_AWARE",
            "WINE_D3D_CONFIG", "FEX_X87REDUCEDPRECISION", "DXVK_LOG_PATH", "DXVK_LOG_LEVEL"}) {
        const char* value=getenv(key); if(value) Set(environment,key,value);
    }
    const char* selected=getenv("STEAMOHOS_PROTON_LOCALE");
    if(selected && (!strcmp(selected,"zh_CN.UTF-8") || !strcmp(selected,"zh_TW.UTF-8") ||
            !strcmp(selected,"ja_JP.UTF-8") || !strcmp(selected,"en_US.UTF-8"))) {
        for(const char* key : {"LANG","LC_ALL","LC_CTYPE","LC_MESSAGES"})Set(environment,key,selected);
        Set(environment,"LOCPATH",code+"/locale");
        Set(environment,"FONTCONFIG_FILE",code+"/tmp/fonts.conf");
    }
''' + '    const char* software=getenv("STEAMOHOS_SOFTWARE_GL");', 1)
(task/'exec_probe.cpp').write_text(variant)
assembly = (fixture/'syscall_compat.S').read_text()
assert assembly.count('    cmp x0, #__NR_openat2') == 1
# On this sandbox optional userfaultfd and futex_waitv probes kill the caller.
# Return the ordinary unsupported-feature error, allowing Wine's real fallback.
# 374 is what this GE ARM64 binary actually issues (confirmed by disassembly);
# 282 is the standard AArch64 userfaultfd number. No syscall is made here.
assembly = assembly.replace('    cmp x0, #__NR_openat2',
    '    cmp x0, #374\n    b.eq 2f\n    cmp x0, #282\n    b.eq 2f\n    cmp x0, #449\n    b.eq 2f\n    cmp x0, #__NR_openat2',1)
(task/'syscall_probe.S').write_text(assembly)
filesystem = (fixture/'filesystem_compat.c').read_text()
filesystem = 'static int CloudLink(const char*,const char*);\n' + filesystem
linkpath = 'static int CloudLinkPath(const char *path,char link[PATH_MAX],char target[PATH_MAX],const char **suffix) {'
assert filesystem.count(linkpath) == 1
filesystem = filesystem.replace(linkpath, linkpath + r'''
    const char *game=getenv("STEAMOHOS_PROTON_GAME_ROOT");
    const char *selected=getenv("STEAMOHOS_PROTON_GAME_DRIVE");
    const char *driveRoot=getenv("STEAMOHOS_PROTON_DRIVE_ROOT");
    if(path && game && selected && strlen(selected)==1 && selected[0]>='a'&&selected[0]<='z' && driveRoot && driveRoot[0]=='/') {
        char drive[PATH_MAX];
        snprintf(drive,sizeof(drive),"/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/dosdevices/%c:",selected[0]);
        if(Below(path,drive)){strcpy(link,drive);strcpy(target,driveRoot);*suffix=path+strlen(drive);return 3;}
    }
    const char *zdrive="/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/dosdevices/z:";
    if(path && game && Below(path,zdrive)) {
        strcpy(link,zdrive);strcpy(target,"/");*suffix=path+strlen(zdrive);return 3;
    }
    const char *cdrive="/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/dosdevices/c:";
    if(path && Below(path,cdrive)) {
        strcpy(link,cdrive);
        strcpy(target,"/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/drive_c");
        *suffix=path+strlen(cdrive);return 3;
    }
''', 1)
symlink = 'int symlink(const char *target,const char *linkpath) {'
assert filesystem.count(symlink) == 1
filesystem = filesystem.replace(symlink, symlink + r'''
    if(!strcmp(target,"../drive_c") &&
       !strcmp(linkpath,"/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/dosdevices/c:"))
        return CloudLink("/data/storage/el2/base/files/steamohos-native/proton-probe/prefix/drive_c",linkpath)?0:-1;
''', 1)
for signature in ['char *realpath(const char *path,char *resolved) {',
                  'char *__realpath_chk(const char *path,char *resolved,size_t capacity) {']:
    assert filesystem.count(signature) == 1
    filesystem = filesystem.replace(signature, signature + r'''
    /* Match the existing readlink executable identity, then let libc perform
     * canonicalization and fortified size checks on the real GNU image. */
    const char *image=getenv("STEAMOHOS_EXEC_PATH");
    if(path && !strcmp(path,"/proc/self/exe") && image && image[0]=='/') path=image;
''', 1)
# Bounded opt-in resource-open evidence, without altering results or retrying.
filesystem = "static void ProtonTraceResource(const char*,int,int);\n" + filesystem
filesystem = filesystem.replace('static void TraceProcAccess(const char *operation,const char *path,int failed,int error) {', r'''
static void ProtonTraceResource(const char *path,int value,int error) {
    if(!getenv("STEAMOHOS_PROTON_GAME_ROOT") ||
       access("/data/storage/el2/base/files/steamohos-native/proton-probe/resource-debug",R_OK))return;
    // Resource successes and failures have their own budget, unaffected by
    // optional Wine module/marker probes during startup. Diagnostic only.
    const char *game=getenv("STEAMOHOS_PROTON_GAME_ROOT");
    int resource=strstr(path,".cpk") || (game && strstr(path,game));
    static unsigned reports, other_reports;
    if(resource) {if(__atomic_fetch_add(&reports,1,__ATOMIC_RELAXED)>=160)return;}
    else if(value>=0 || __atomic_fetch_add(&other_reports,1,__ATOMIC_RELAXED)>=24)return;
    fprintf(stderr,"HMOSDECK_RESOURCE_OPEN pid=%d fd=%d errno=%d path=%.1000s\n",getpid(),value,value<0?error:0,path);
}
''' + 'static void TraceProcAccess(const char *operation,const char *path,int failed,int error) {', 1)
filesystem = filesystem.replace('        TraceProcAccess(#name,path,value<0,saved); errno=saved; return value;',
    '        ProtonTraceResource(path,value,saved); TraceProcAccess(#name,path,value<0,saved); errno=saved; return value;', 1)
(task/'filesystem_probe.c').write_text(filesystem)
posix = (fixture/'posix_compat.c').read_text()
posix = posix.replace('#define _GNU_SOURCE 1', '#define _GNU_SOURCE 1\n#include <sys/prctl.h>', 1)
# OHOS executable-memory policy belongs to the calling thread. Wine's Steam
# helper creates the game from another thread, and fork does not establish the
# new child's policy. Request the existing policy, then preserve the kernel's
# actual mprotect result. Never mask a failed executable-file protection.
protect = '    int result=next_mprotect(address,length,prot),saved=errno;'
assert posix.count(protect) == 1
posix = posix.replace('#include <sys/prctl.h>', (fixture/'proton_exec_policy.h').read_text(), 1)
posix = posix.replace('#define _GNU_SOURCE 1', '#define _GNU_SOURCE 1\n' +
                      (fixture/'proton_file_mapping.h').read_text(), 1)
posix = posix.replace(protect, '    proton_prepare_exec_policy(prot);\n' + protect + r"""
    if(result < 0 && (prot & PROT_EXEC) &&
       !access("/data/storage/el2/base/files/steamohos-native/proton-probe/memory-map-debug", R_OK)) {
        static _Atomic unsigned map_failures;
        if(atomic_fetch_add(&map_failures,1)<8) {
            FILE *maps=fopen("/proc/self/maps","r");
            char line[1024]; unsigned records=0;
            while(maps && records<16 && fgets(line,sizeof(line),maps)) {
                unsigned long low=0,high=0;
                if(sscanf(line,"%lx-%lx",&low,&high)==2 &&
                   low<(uintptr_t)address+length && high>(uintptr_t)address) {
                    fprintf(stderr,"HMOSDECK_PROTECT_VIEW pid=%d requested=%p+%zu %s",getpid(),address,length,line);
                    ++records;
                }
            }
            if(maps)fclose(maps);
        }
    }
""", 1)
# Keep failure diagnostics, without filling the bounded result with optional
# builtin .so lookups (modern Wine implements these modules as PE files).
posix = posix.replace('atomic_fetch_add(&diagnostics,1)<50','atomic_fetch_add(&diagnostics,1)<2')
posix = posix.replace('atomic_fetch_add(&failures,1)<20','atomic_fetch_add(&failures,1)<1')
mmap = 'void *mmap(void *address,size_t length,int prot,int flags,int fd,off_t offset) {'
assert posix.count(mmap) == 1
posix = posix.replace(mmap, mmap + r'''
    // This sandbox permits anonymous executable pages but not executable
    // file mappings. Wine already supports private anonymous+pread views on ENODEV;
    // use that real fallback for this runtime and the selected game directory.
    // Shared mappings and product paths retain their exact behavior.
    if(fd>=0 && (flags&(MAP_PRIVATE|MAP_FIXED))==(MAP_PRIVATE|MAP_FIXED) &&
       !(flags&(MAP_SHARED|MAP_ANONYMOUS))) {
        char descriptor[64],path[PATH_MAX];
        snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
        ssize_t count=readlink(descriptor,path,sizeof(path)-1);
        if(count>0) {
            path[count]=0;
            const char *game=getenv("STEAMOHOS_PROTON_GAME_ROOT");
            int fallback=proton_private_read_fallback(fd,flags,path,game);
            if(!access("/data/storage/el2/base/files/steamohos-native/proton-probe/pe-map-debug",R_OK)) {
                static _Atomic unsigned map_logged;
                if(atomic_fetch_add(&map_logged,1)<60)
                    fprintf(stderr,"HMOSDECK_PROTON_PE_MAP offset=%lld prot=%d flags=%x fallback=%d file=%.240s\n",(long long)offset,prot,flags,fallback,path);
            }
            if(fallback) {
                static _Atomic unsigned pe_logged;
                if(atomic_fetch_add(&pe_logged,1)<2)
                    fprintf(stderr,"HMOSDECK_PROTON_PE_READ_FALLBACK offset=%lld file=%.180s\n",(long long)offset,path);
                errno=ENODEV;return MAP_FAILED;
            }
        }
    }
''', 1)
# Observe real game-data mappings, including failed non-fixed initial views.
# Keep errors and flags unchanged. Independent budget avoids startup PE noise.
posix = posix.replace('    void *result=next_mmap(address,length,prot,flags,fd,offset);', r'''
    char resource_path[PATH_MAX]={0};
    if(fd>=0 && !access("/data/storage/el2/base/files/steamohos-native/proton-probe/resource-debug",R_OK)) {
        char descriptor[64];snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
        ssize_t count=readlink(descriptor,resource_path,sizeof(resource_path)-1);
        if(count>0)resource_path[count]=0;
    }
''' + '    void *result=next_mmap(address,length,prot,flags,fd,offset);', 1)
posix = posix.replace('    int saved=errno;\n    static _Atomic unsigned diagnostics;', r'''
    int saved=errno;
    if(strstr(resource_path,".cpk")) {
        static _Atomic unsigned resource_maps;
        if(atomic_fetch_add(&resource_maps,1)<64)
            fprintf(stderr,"HMOSDECK_RESOURCE_MAP pid=%d fd=%d offset=%lld length=%zu prot=%x flags=%x result=%p errno=%d file=%.1000s\n",
                getpid(),fd,(long long)offset,length,prot,flags,result,result==MAP_FAILED?saved:0,resource_path);
    }
    static _Atomic unsigned diagnostics;
''', 1)
signed_library = 'static const char *SignedLibrary(const char *name, char *mapped, size_t capacity) {'
assert posix.count(signed_library) == 1
posix = posix.replace(signed_library, signed_library + r'''
    // Mesa 25 separates Gallium from the upstream DRIL compatibility DSO
    // used by Xwayland's GLX provider. Resolve the real DRI entry point on a
    // sandbox filesystem that does not support Unix symlinks.
    const char *probe="/data/storage/el2/base/files/steamohos-native/proton-probe";
    const char *probeCode=getenv("STEAMOHOS_CODE_ROOT");
    if(name&&probeCode&&!strcmp(probeCode,probe)) {
        char alias[PATH_MAX];
        // Exact mmdevapi Unix ABI is checked by the direct-audio builder.
        // Retain GE's PE driver and original package; only this explicit
        // private experiment can load the shared OHOS Wine implementation.
        const char *audioName=strrchr(name,'/');audioName=audioName?audioName+1:name;
        if(!strcmp(audioName,"winealsa.so") &&
           snprintf(alias,sizeof(alias),"%s/audio-direct",probe)<(int)sizeof(alias) && !access(alias,R_OK)) {
            if(snprintf(mapped,capacity,"%s/libwineohos-gnu.so",probe)>=(int)capacity)return name;
            return mapped;
        }
        if(!strcmp(name,"libvulkan.so.1") && getenv("STEAMOHOS_PROTON_NATIVE_VULKAN")) {
            if(snprintf(mapped,capacity,"%s/graphics/libproton_vulkan.so",probe)>=(int)capacity)return name;
            return mapped;
        }
        if(!strcmp(name,"libEGL.so.1") && getenv("STEAMOHOS_PROTON_NATIVE_EGL")) {
            if(snprintf(mapped,capacity,"%s/graphics/libproton_egl.so",probe)>=(int)capacity)return name;
            return mapped;
        }
        if(snprintf(alias,sizeof(alias),"%s/graphics/swrast_dri.so",probe)<(int)sizeof(alias)&&!strcmp(name,alias)) {
            if(snprintf(mapped,capacity,"%s/graphics/libdril_dri.so",probe)>=(int)capacity)return name;
            return mapped;
        }
    }
''', 1)
posix += r"""
#include <fcntl.h>
#include <locale.h>
#include <langinfo.h>
// Bounded, explicit diagnostic switch for this private experiment only.
__attribute__((constructor)) static void ProtonDiagnostic(int argc, char **argv, char **envp) {
    (void)envp;
    const char *root=getenv("STEAMOHOS_CODE_ROOT");
    if(!root||strcmp(root,"/data/storage/el2/base/files/steamohos-native/proton-probe"))return;
    if(!access("/data/storage/el2/base/files/steamohos-native/proton-probe/memory-map-debug",R_OK)) {
        const char *game=getenv("STEAMOHOS_PROTON_GAME_ROOT"), *drive=getenv("STEAMOHOS_PROTON_GAME_DRIVE");
        const char *path=getenv("WINEPATH"), *egl=getenv("STEAMOHOS_PROTON_NATIVE_EGL");
        char cwd[PATH_MAX];
        fprintf(stderr,"HMOSDECK_PROTON_CONTEXT pid=%d generation=%s game_root=%d drive=%s winepath=%d egl=%d cwd=%s\n",
            getpid(),getenv("STEAMOHOS_EXEC_GENERATION")?getenv("STEAMOHOS_EXEC_GENERATION"):"0",
            game&&*game,drive?drive:"none",path&&*path,egl&&*egl,getcwd(cwd,sizeof(cwd))?cwd:"unavailable");
    }
    const char *selected=getenv("STEAMOHOS_PROTON_LOCALE");
    if(!selected)selected=getenv("LC_ALL");
    if(selected && (!strcmp(selected,"zh_CN.UTF-8") || !strcmp(selected,"zh_TW.UTF-8") ||
            !strcmp(selected,"ja_JP.UTF-8") || !strcmp(selected,"en_US.UTF-8"))) {
        char saved[32];snprintf(saved,sizeof(saved),"%s",selected);
        setenv("STEAMOHOS_PROTON_LOCALE",saved,1);
        for(const char** key=(const char*[]){"LANG","LC_ALL","LC_CTYPE","LC_MESSAGES",NULL};*key;++key)
            setenv(*key,saved,1);
        setenv("LOCPATH","/data/storage/el2/base/files/steamohos-native/proton-probe/locale",1);
        setenv("FONTCONFIG_FILE","/data/storage/el2/base/files/steamohos-native/proton-probe/tmp/fonts.conf",1);
    }
    locale_t check=newlocale(LC_CTYPE_MASK,"",(locale_t)0);
    fprintf(stderr,"HMOSDECK_PROTON_LOCALE pid=%d requested=%s loaded=%d codeset=%s\n",
        getpid(),getenv("LC_ALL")?getenv("LC_ALL"):"none",check!=0,check?nl_langinfo_l(CODESET,check):"unavailable");
    if(check)freelocale(check);
    // GE enables the LAA override even for legacy non-LAA executables.
    // Keep Windows' PE-header contract: 0 disables only the forced override,
    // while binaries declaring IMAGE_FILE_LARGE_ADDRESS_AWARE still get 4 GiB.
    // Explicit user settings keep their value; no game-specific exception.
    if(!getenv("WINE_LARGE_ADDRESS_AWARE"))setenv("WINE_LARGE_ADDRESS_AWARE","0",1);
    // Match the working OHOS Wine ARM runtime's native-F64 x87 policy.
    // GE's standalone FEX defaults to software F80; the 32-bit VLC decoder
    // then misses deadlines even with no audio output. Preserve an explicit
    // caller override (0 selects strict F80) instead of overriding game policy.
    if(!getenv("FEX_X87REDUCEDPRECISION"))setenv("FEX_X87REDUCEDPRECISION","1",0);
    int vlc_probe=open("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-vlc",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(vlc_probe>=0) {
        close(vlc_probe);
        for(int n=0;argv&&n<argc;n++)if(argv[n]&&strstr(argv[n],"proton-x64-media.exe"))
            setenv("STEAMOHOS_PROTON_GAME_ROOT","/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/NewPAL",1);
        // The game trial has a bounded ten-minute budget, allowing the
        // independent 65-second VLC diagnostic to exit normally. Do not
        // change game content or the standard short audio fixture budget.
        for(int n=0;argv&&n<argc;n++)if(argv[n]&&!strcmp(argv[n],"G:\\NewPAL_Release.exe")) {
            argv[n]="C:\\proton-x86-vlc.exe";
            fprintf(stderr,"HMOSDECK_VLC_TRIAL game_replaced_by_fixture=1 game_files_modified=0\n");
        }
    }
    // Original Valve/GE Wine gates the Wayland OpenGL driver behind this
    // opt-in. Scope it to the explicitly selected shared-EGL experiment.
    if(getenv("STEAMOHOS_PROTON_NATIVE_EGL")) {
        setenv("WINE_USE_EGL","1",1);
        // Match the product Wine capability guard for the current GL 3.1
        // virgl profile. GE's optional fullscreen gamma shader needs GLSL
        // 330; the shared receiver already presents/scales the drawable.
        // This upstream switch disables that postprocess, not GPU rendering.
        setenv("WINE_DISABLE_FULLSCREEN_HACK","1",1);
        // An EGL SoC without GL_NVX_gpu_memory_info reports unknown/zero
        // dedicated VRAM to GE Wine. Its generic WineD3D adapter then rejects
        // even a 640x480 texture. Use Wine's existing UMA texture-budget
        // setting, bounded by RAM, until the GNU renderer query has a budget.
        // This is accounting only: no GL version/extension is advertised.
        const char *configured=getenv("WINE_D3D_CONFIG");
        if(!configured||!strstr(configured,"VideoMemorySize=")) {
            const long pages=sysconf(_SC_PHYS_PAGES), pageSize=sysconf(_SC_PAGESIZE);
            if(pages>0&&pageSize>0) {
                uint64_t ramMiB=(uint64_t)pages*(uint64_t)pageSize/(1024*1024);
                uint64_t budget=ramMiB/4;if(budget>2048)budget=2048;
                if(budget) {
                    char config[4096];
                    int count=snprintf(config,sizeof(config),"%s%sVideoMemorySize=%llu",
                        configured?configured:"",configured&&*configured?";":"",
                        (unsigned long long)budget);
                    if(count>0&&count<(int)sizeof(config)) {
                        setenv("WINE_D3D_CONFIG",config,1);
                        fprintf(stderr,"HMOSDECK_PROTON_UMA_BUDGET ram_mib=%llu texture_mib=%llu source=wine_config\n",
                            (unsigned long long)ramMiB,(unsigned long long)budget);
                    }
                }
            }
        }
    }
    // The combined D3D fixture imports d3d9 even in its D3D11 mode.
    // Keep that unused import builtin; only d3d11/dxgi use the test DXVK DLLs.
    if(getenv("STEAMOHOS_PROTON_NATIVE_VULKAN"))setenv("STEAMOHOS_PROTON_VULKAN_MAP32","1",1);
    if(!access("/data/storage/el2/base/files/steamohos-native/proton-probe/font-debug",R_OK))
        setenv("WINEDEBUG","-all,err+all,trace+font,trace+text",1);
    int gpu=open("/data/storage/el2/base/files/steamohos-native/proton-probe/gpu-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(gpu>=0){close(gpu);setenv("MESA_DEBUG","1",1);setenv("WINEDEBUG","-all,err+all,warn+opengl,warn+waylanddrv",1);}
    if(!access("/data/storage/el2/base/files/steamohos-native/proton-probe/resource-debug",R_OK))
        setenv("WINEDEBUG","-all,err+all,trace+file,trace+seh",1);
    int fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/module-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){close(fd);setenv("WINEDEBUG","-all,err+all,warn+module,trace+module",1);}
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/media-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){close(fd);setenv("WINEDEBUG","-all,err+all,warn+alsa,trace+alsa,trace+mmdevapi,trace+winebus,trace+service",1);}
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/init-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){close(fd);setenv("WINEDEBUG","-all,err+all,trace+setupapi,trace+service",1);}
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/steam-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){
        close(fd);setenv("WINEDEBUG","-all,err+all,trace+steam,warn+steamclient,warn+module,trace+loaddll",1);
        // Independent explicit dlopen probe owns its own dlerror. Do not
        // consume the error from a later lsteamclient call.
        if(getenv("STEAMOHOS_PROTON_GAME_ROOT") && !access("/data/storage/el2/base/files/steamohos-native/proton-probe/sdk-load-debug",R_OK)) {
            const char *sdk="/data/storage/el2/base/files/steamohos-native/proton-probe/home/.steam/sdkarm64/steamclient.so";
            void *client=dlopen(sdk,RTLD_NOW|RTLD_LOCAL);
            fprintf(stderr,"HMOSDECK_PROTON_SDK_LOAD success=%d error=%s\n",client!=NULL,client?"none":dlerror());
        }
    }
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/game-init-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){
        close(fd);
        // SDL's Windows logger uses OutputDebugString. Keep this opt-in and
        // bounded by the existing probe log rotation; no per-frame tracing.
        setenv("WINEDEBUG","-all,err+all,warn+steamclient,warn+module,trace+debugstr,warn+d3d,trace+mmdevapi,trace+alsa",1);
    }
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-flow-debug",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){
        close(fd);
        setenv("WINEDEBUG","-all,err+all,warn+dsound,warn+ohosaudio,trace+debugstr",1);
        // Windows VLC reports its output-clock corrections through
        // OutputDebugString; retain these only for this bounded experiment.
        setenv("VLC_VERBOSE","2",1);
    }
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-dsound-trace",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        close(fd);
        int game=0;
        for(int n=0;argv&&n<argc;n++)
            if(argv[n]&&strstr(argv[n],"NewPAL_Release.exe"))game=1;
        if(game)setenv("WINEDEBUG","-all,err+all,trace+dsound,trace+timestamp",1);
    }
    // Diagnostic comparison only: let the unchanged VLC/SDL libraries choose
    // their next supported Windows audio API. Registration still runs with
    // DirectSound enabled; no game files or default product route are changed.
    fd=open("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-no-dsound",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        close(fd);
        int game=0;
        for(int n=0;argv&&n<argc;n++)
            if(argv[n]&&strstr(argv[n],"NewPAL_Release.exe"))game=1;
        if(game) {
            const char *previous=getenv("WINEDLLOVERRIDES");
            char overrides[8192];
            int size=snprintf(overrides,sizeof(overrides),"%s%sdsound=d",previous?previous:"",previous&&*previous?";":"");
            if(size>0&&size<(int)sizeof(overrides)) {
                setenv("WINEDLLOVERRIDES",overrides,1);
                fprintf(stderr,"HMOSDECK_PROTON_AUDIO_ROUTE dsound_disabled=1 comparison_only=1\n");
            }
        }
    }
}
"""
(task/'posix_probe.c').write_text(posix)
objects = [base/(name+'.o') for name in ['sysvipc_compat.c',
    'popen_compat.c','socket_metadata.c','native_loader.cpp','native_loader_entry.S']]
binary = task/'libproton_probe.so'
inputs = {'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in
    [task/'exec_probe.cpp',task/'filesystem_probe.c',task/'posix_probe.c',task/'syscall_probe.S']},
    'baseManifest':hashlib.sha256((base/'runtime/manifest.json').read_bytes()).hexdigest(),
    'objects':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in objects},
    'compilers':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [cc,cxx]}}
identity = task/'posix-probe-identity.json'
if identity.exists() and binary.exists():
    previous = json.loads(identity.read_text())
    if previous.get('buildInputs') == inputs and previous.get('sha256') == hashlib.sha256(binary.read_bytes()).hexdigest():
        print('Reused verified isolated Proton shim')
        raise SystemExit(0)
subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-fPIC',
    '-I'+str(fixture),'-c',str(task/'filesystem_probe.c'),'-o',str(task/'filesystem_probe.o')],check=True)
subprocess.run([str(cc),'-O2','-Wall','-Wextra','-Werror','-fPIC',
    '-I'+str(fixture),'-c',str(task/'posix_probe.c'),'-o',str(task/'posix_probe.o')],check=True)
for source in [task/'exec_probe.cpp',task/'syscall_probe.S']:
    subprocess.run([str(cxx),'-O2','-Wall','-Wextra','-Werror','-std=c++17','-fPIC',
        '-I'+str(fixture),'-c',str(source),'-o',str(source.with_suffix('.o'))],check=True)
objects = [base/(name+'.o') for name in ['sysvipc_compat.c',
    'popen_compat.c','socket_metadata.c','native_loader.cpp','native_loader_entry.S']]
binary = task/'libproton_probe.so'
subprocess.run([str(cxx),'-shared',str(task/'exec_probe.o'),str(task/'syscall_probe.o'),
    str(task/'filesystem_probe.o'),
    str(task/'posix_probe.o'),
    *map(str,objects),'-pthread','-ldl','-Wl,-z,relro,-z,now','-Wl,-soname,libproton_probe.so',
    '-o',str(binary)],check=True)
record = {'buildInputs':inputs,'name':binary.name,'bytes':binary.stat().st_size,
    'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(), 'baseManifest':str(base/'runtime/manifest.json'),
    'delta':'Isolated optional syscall fallbacks, fork spawn, preloader skip, executable identity, C drive records, private PE read fallback, bounded desktop probe',
    'generatedSources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [task/'exec_probe.cpp',task/'filesystem_probe.c',task/'posix_probe.c',task/'syscall_probe.S']},
    'baseManifestSha256':hashlib.sha256((base/'runtime/manifest.json').read_bytes()).hexdigest(),
    'productRuntimeModified':False}
(task/'posix-probe-identity.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record),flush=True)
