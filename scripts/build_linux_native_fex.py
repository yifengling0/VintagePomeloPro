#!/usr/bin/env python3
"""Build the pinned GNU game FEX core from reviewed production inputs.

The fixed source/build directory is reusable only with matching source,
patch, toolchain and ABI identities. Valve depots are never modified.
"""
import argparse
import hashlib
import io
import json
import os
import re
import tempfile
from pathlib import Path
from linux_component_identity import tree_identity
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = '37265b109a4bba9e10241972dfcb3d1fdb8f3f9a'
SUBMODULES = ['External/fmt', 'External/range-v3', 'External/unordered_dense',
              'External/xxhash', 'Source/Common/cpp-optparse',
              'External/jemalloc_glibc', 'External/rpmalloc', 'External/drm-headers']
DOWNLOADS = {
    'External/jemalloc_glibc': ('FEX-Emu/jemalloc', '800cc0653f5bc2f0ed2cc7bb383f8eeda1ff94b2e89eaee0ccc57bd5fb002fe4'),
    'External/rpmalloc': ('FEX-Emu/rpmalloc', '201b7f48b1e0202967b816b833261d8aea2a4943e0da92c82107a54c0b89f62c'),
    'External/drm-headers': ('FEX-Emu/drm-headers', '3d3653841f81ea5be1a15cd4b458f048c1fffbcd97fcc808454a7fbe7cf1a555'),
}
PATCH_FILES = ['FEXCore/Source/Interface/Core/OpcodeDispatcher.cpp',
               'FEXCore/Source/Interface/Core/OpcodeDispatcher.h',
               'Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp',
               'Source/Tools/LinuxEmulation/LinuxSyscalls/SyscallsSMCTracking.cpp']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(repository, *args):
    return subprocess.check_output(['git', '-C', str(repository), *args])


def extract(stream, destination, strip=False):
    with tarfile.open(fileobj=io.BytesIO(stream)) as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or member.islnk():
                raise ValueError('Unsafe source archive')
            if strip:
                if len(path.parts) < 2:
                    continue
                member.name = str(Path(*path.parts[1:]))
            if member.issym():
                target = Path(member.linkname)
                if target.is_absolute() or '..' in target.parts:
                    raise ValueError('Unsafe source link')
            archive.extract(member, destination, filter='data')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--gnu-toolchain', type=Path, required=True)
    parser.add_argument('--linux-uapi', type=Path, required=True, help='Directory containing linux/ and asm-generic/')
    parser.add_argument('--force-rebuild', action='store_true', help='Recompile all objects to audit reproducibility')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--download-missing', action='store_true')
    parser.add_argument('--stock-control', action='store_true', default=True)
    parser.add_argument('--upstream-sigreturn', action='store_true', default=True,
                        help='Apply the exact existing diff published in FEX issue 5901')
    parser.add_argument('--ohos-guest-protection', action='store_true', default=True,
                        help='Separate x86 guest permissions from host noexec memfd policy')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error('Use 1-16 jobs')
    repository = args.repository.resolve(strict=True)
    git(repository, 'cat-file', '-e', REVISION + '^{commit}')
    clang = args.clang.resolve(strict=True)
    gnu = args.gnu_toolchain.resolve(strict=True)
    uapi = args.linux_uapi.resolve(strict=True)
    if not (uapi / 'linux/types.h').is_file():
        parser.error('Linux UAPI headers required')
    output = args.output_dir.resolve()
    source = output / 'source'
    if source == repository or repository in source.parents:
        parser.error('Keep the build outside the shared source repository')
    output.mkdir(parents=True, exist_ok=True)
    lock_path = output / 'source-lock.json'
    if source.exists():
        if not lock_path.exists() or json.loads(lock_path.read_text()).get('revision') != REVISION:
            parser.error('Existing private source has no matching revision lock')
    else:
        extract(git(repository, 'archive', REVISION), source)
    # Reconstruct every input from immutable git objects / checked archives.
    # Only patched files need a temporary working copy. Unpatched cache edits
    # are rejected, never silently accepted as a new production baseline.
    patch = ROOT / 'scripts/patches/fex-valve-2607-pop-memory-precise-stack.patch'
    contract = ROOT / 'scripts/patches/fex-valve-2607-game-contract.patch'
    reproducible = ROOT / 'scripts/patches/fex-reproducible-thunkgen.patch'
    upstream_patch = ROOT / 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch'
    upstream_sha = 'de76b76140e2933a574ea5427b372aa2512980c694a9aa37f9aac8f37708ee3e'
    guest_patch = ROOT / 'scripts/patches/fex-ohos-guest-protection.patch'
    guest_header = ROOT / 'scripts/patches/fex-ohos-guest-protection.h'
    patches = ([] if args.stock_control else [patch])
    if args.upstream_sigreturn:
        if sha(upstream_patch) != upstream_sha:
            raise ValueError('Published upstream patch checksum mismatch')
        patches.append(upstream_patch)
    if args.ohos_guest_protection:
        if not args.stock_control or not args.upstream_sigreturn:
            parser.error('OHOS guest protection requires the reviewed sigreturn baseline')
        patches.append(guest_patch)
    patches += [contract, reproducible]
    touched = set()
    for item in patches:
        touched.update(re.findall(r'^\+\+\+ b/(.+)$', item.read_text(), re.M))
    touched.add('Source/Tools/LinuxEmulation/LinuxSyscalls/OhosGuestProtection.h')
    lock = {'revision': REVISION, 'submodules': {}}
    archives = [('', git(repository, 'archive', REVISION), False)]
    for name in SUBMODULES:
        metadata = git(repository, 'ls-tree', REVISION, name).decode().split()
        if len(metadata) != 4 or metadata[0] != '160000':
            raise RuntimeError('Missing pinned submodule ' + name)
        revision = metadata[2]
        lock['submodules'][name] = revision
        checkout = repository / name
        have_object = subprocess.run(['git', '-C', str(checkout), 'cat-file', '-e', revision + '^{commit}'],
                                     capture_output=True).returncode == 0
        if have_object:
            archives.append((name, git(checkout, 'archive', revision), False))
        else:
            if name not in DOWNLOADS:
                raise RuntimeError('Initialize pinned dependency first: ' + name)
            repo_url, expected = DOWNLOADS[name]
            archive = output / (name.rsplit('/', 1)[1] + '.tar.gz')
            if not archive.exists():
                if not args.download_missing:
                    raise RuntimeError('Initialize pinned dependency first: ' + name)
                request = urllib.request.Request('https://codeload.github.com/' + repo_url + '/tar.gz/' + revision,
                                                 headers={'User-Agent': 'VintagePomelo-FEX-build'})
                with urllib.request.urlopen(request, timeout=45) as response:
                    data = response.read(32 * 1024 * 1024 + 1)
                if len(data) > 32 * 1024 * 1024:
                    raise ValueError('Dependency download size limit')
                archive.write_bytes(data)
            if sha(archive) != expected:
                raise ValueError('Dependency archive checksum mismatch')
            archives.append((name, archive.read_bytes(), True))
    expected_sources = set(touched)
    with tempfile.TemporaryDirectory(prefix='patch-inputs-', dir=output) as temporary:
        patched = Path(temporary)
        for prefix, data, strip in archives:
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                for member in archive:
                    parts = Path(member.name).parts[1:] if strip else Path(member.name).parts
                    if not parts or member.isdir():
                        continue
                    if Path(member.name).is_absolute() or '..' in parts or member.islnk():
                        raise ValueError('Unsafe pinned source archive')
                    relative = str(Path(prefix, *parts))
                    expected_sources.add(relative)
                    target = (patched if relative in touched else source) / relative
                    if any(parent.is_symlink() for parent in target.parents if parent != source and parent.is_relative_to(source)):
                        raise ValueError('Source cache traverses an external symlink: ' + relative)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if member.issym():
                        link = Path(member.linkname)
                        if link.is_absolute() or '..' in link.parts:
                            raise ValueError('Unsafe pinned source link')
                        if target.is_symlink():
                            if os.readlink(target) != member.linkname:
                                raise ValueError('Modified source symlink: ' + relative)
                        elif target.exists():
                            raise ValueError('Expected source symlink: ' + relative)
                        else:
                            target.symlink_to(member.linkname)
                    elif member.isfile():
                        content = archive.extractfile(member).read()
                        if target.is_symlink():
                            raise ValueError('Unexpected source symlink: ' + relative)
                        if target.exists() and target.read_bytes() != content:
                            if target.read_bytes().replace(b'\r\n', b'\n') != content:
                                raise ValueError('Modified unpatched source cache: ' + relative)
                            target.write_bytes(content)
                        if not target.exists():
                            target.write_bytes(content)
                            target.chmod(member.mode)
        for item in patches:
            subprocess.run(['patch', '-p1', '--fuzz=0', '--batch', '--no-backup-if-mismatch', '-i', str(item)],
                           cwd=patched, check=True, stdout=subprocess.DEVNULL)
        if args.ohos_guest_protection:
            (patched / 'Source/Tools/LinuxEmulation/LinuxSyscalls/OhosGuestProtection.h').write_bytes(guest_header.read_bytes())
        for relative in touched:
            content = (patched / relative).read_bytes()
            destination = source / relative
            if destination.is_symlink():
                raise ValueError('Unexpected patched source symlink: ' + relative)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists() or destination.read_bytes() != content:
                destination.write_bytes(content)
    for path in source.rglob('*'):
        if (path.is_file() or path.is_symlink()) and path.relative_to(source).as_posix() not in expected_sources:
            raise ValueError('Unexpected file in private source cache: ' + str(path.relative_to(source)))
    lock_path.write_text(json.dumps(lock, indent=2) + '\n')
    epoch = int(git(repository, 'show', '-s', '--format=%ct', REVISION).strip())
    build_env = os.environ | {'SOURCE_DATE_EPOCH': str(epoch), 'TZ': 'UTC', 'LC_ALL': 'C'}
    version = ('FEX-2607-g37265b1-vpp-' + ('sigret' if args.stock_control else 'pop-sr')) if args.upstream_sigreturn else ('FEX-2607-76-g37265b1-vpp-' + ('stock' if args.stock_control else 'pop'))
    flags = (f'--target=aarch64-none-linux-gnu --gcc-toolchain={gnu} '
             f'--sysroot={gnu}/aarch64-none-linux-gnu/libc -isystem {uapi} '
             f'-isystem {uapi}/aarch64-linux-gnu -ffile-prefix-map={source}=/usr/src/fex')
    toolchain = output / 'gnu-toolchain.cmake'
    toolchain.write_text('\n'.join([
        'set(CMAKE_SYSTEM_NAME Linux)', 'set(CMAKE_SYSTEM_PROCESSOR aarch64)',
        f'set(CMAKE_C_COMPILER "{clang}")', f'set(CMAKE_CXX_COMPILER "{clang.parent}/clang++")',
        f'set(CMAKE_ASM_COMPILER "{clang}")',
        *[f'set(CMAKE_{language}_FLAGS_INIT "{flags}")' for language in ['C', 'CXX', 'ASM']],
        'set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")',
        f'set(CMAKE_AR "{clang.parent}/llvm-ar")', f'set(CMAKE_RANLIB "{clang.parent}/llvm-ranlib")',
        f'set(CMAKE_FIND_ROOT_PATH "{gnu}/aarch64-none-linux-gnu/libc")',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)', 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)', 'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)', '']))
    options = ['-DCMAKE_BUILD_TYPE=Release', '-DBUILD_FEXCONFIG=OFF', '-DBUILD_FEX_LINUX_TESTS=OFF',
        '-DBUILD_STEAM_SUPPORT=ON', '-DBUILD_TESTING=OFF', '-DBUILD_THUNKS=OFF', '-DFEX_HSL_USER_PROCESS=ON', '-DENABLE_LTO=OFF',
        '-DENABLE_LIBCXX=OFF', '-DENABLE_GDB_SYMBOLS=OFF', '-DENABLE_JEMALLOC_GLIBC_ALLOC=ON',
        '-DENABLE_FEX_ALLOCATOR=ON', '-DENABLE_OFFLINE_TELEMETRY=OFF', '-DENABLE_VIXL_DISASSEMBLER=OFF',
        '-DENABLE_ZYDIS=OFF', '-DTUNE_CPU=generic',
        '-DOVERRIDE_VERSION=' + version,
        '-DOVERRIDE_HASH=' + REVISION]
    inputs = dict(source=lock, sourceDateEpoch=epoch, reproducibleThunkgenPatchSha256=sha(reproducible), builderSha256=sha(Path(__file__)), gameContractSha256=sha(contract),
                  gameContractProvenanceSha256=sha(contract.with_suffix('.provenance.json')),
                  compilerVersion=subprocess.check_output([clang, '--version'], text=True).splitlines()[0],
                  gnuCompilerSha256=sha(gnu/'bin/aarch64-none-linux-gnu-gcc'),
                  toolchainClosure=tree_identity([('sysroot',gnu/'aarch64-none-linux-gnu'),('gcc',gnu/'lib/gcc'),
                    ('clang',clang),('clang++',clang.parent/'clang++'),('lld',clang.parent/'ld.lld'),
                    ('ar',clang.parent/'llvm-ar'),('ranlib',clang.parent/'llvm-ranlib')]),
                  identityScriptSha256=sha(ROOT/'scripts/linux_component_identity.py'),
                  uapiIdentity={str(p.relative_to(uapi)):sha(p) for p in sorted(uapi.rglob('*.h')) if p.is_file()}, patchSha256=None if args.stock_control else sha(patch),
                  compilerSha256=sha(clang), toolchainSha256=sha(toolchain), options=options,
                  upstreamSigreturnPatchSha256=upstream_sha if args.upstream_sigreturn else None,
                  ohosGuestProtectionPatchSha256=sha(guest_patch) if args.ohos_guest_protection else None,
                  ohosGuestProtectionHeaderSha256=sha(guest_header) if args.ohos_guest_protection else None)
    manifest = output / ('sigreturn-manifest.json' if args.upstream_sigreturn else 'stock-manifest.json' if args.stock_control else 'manifest.json')
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    tree = lambda: {str(p.relative_to(source)):sha(p) for p in sorted(source.rglob('*')) if p.is_file() and (p.suffix in ('.h', '.cpp', '.c', '.S', '.cmake', '.inl') or p.name == 'CMakeLists.txt')}
    source_identity = tree()
    if not args.force_rebuild and old.get('inputs') == inputs and old.get('sourceTree') == source_identity and all((output/'build/Bin'/n).is_file() and old.get('executables',{}).get(n,{}).get('sha256') == sha(output/'build/Bin'/n) for n in ('FEX','FEXServer')):
        print('Reused matching FEX build; SHA-256 verified')
        return
    commands = [('configure', ['cmake', '-S', source, '-B', output / 'build', '-G', 'Ninja',
                                '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), *options]),
                ('build', ['cmake', '--build', output / 'build', '--target', 'FEX', 'FEXServer', '--parallel', str(args.jobs), *(['--clean-first'] if args.force_rebuild else [])])]
    for name, command in commands:
        with (output / (name + '.log')).open('w') as log:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT, env=build_env)
        if result.returncode:
            print((output / (name + '.log')).read_text()[-8000:])
            raise SystemExit(result.returncode)
    if tree() != source_identity: raise ValueError('FEX sources changed during build')
    record = dict(source=lock, sourceTree=source_identity, patchSha256=inputs['patchSha256'],
                  upstreamSigreturnPatchSha256=inputs['upstreamSigreturnPatchSha256'], inputs=inputs, executables={})
    for name in ['FEX', 'FEXServer']:
        path = output / 'build/Bin' / name
        record['executables'][name] = dict(bytes=path.stat().st_size, sha256=sha(path))
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record['executables'], indent=2))


if __name__ == '__main__':
    main()
