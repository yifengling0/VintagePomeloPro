#!/usr/bin/env python3
"""Build the matching Valve FEX core with precise memory-POP fault state.

Diagnostic only: no automatic installation or replacement of Steam's tool.
The original repository and the paired thunks remain untouched. Reuses a fixed
source/build directory; --stock-control removes only this patch for A/B.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
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
               'Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator/GuestFramesManagement.cpp']


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
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--download-missing', action='store_true')
    parser.add_argument('--stock-control', action='store_true')
    parser.add_argument('--upstream-sigreturn', action='store_true',
                        help='Apply the exact existing diff published in FEX issue 5901')
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
    lock = {'revision': REVISION, 'submodules': {}}
    for name in SUBMODULES:
        metadata = git(repository, 'ls-tree', REVISION, name).decode().split()
        if len(metadata) != 4 or metadata[0] != '160000':
            raise RuntimeError('Missing pinned submodule ' + name)
        revision = metadata[2]
        lock['submodules'][name] = revision
        destination = source / name
        if destination.is_dir() and any(destination.iterdir()):
            # Existing private checkout/export was prepared with this same lock.
            old = json.loads(lock_path.read_text())
            if old.get('submodules', {}).get(name) != revision:
                raise RuntimeError('Existing dependency revision mismatch: ' + name)
            continue
        checkout = repository / name
        have_object = subprocess.run(['git', '-C', str(checkout), 'cat-file', '-e', revision + '^{commit}'],
                                     capture_output=True).returncode == 0
        if have_object:
            extract(git(checkout, 'archive', revision), destination)
        else:
            if not args.download_missing or name not in DOWNLOADS:
                raise RuntimeError('Initialize pinned dependency first: ' + name)
            repo_url, expected = DOWNLOADS[name]
            archive = output / (name.rsplit('/', 1)[1] + '.tar.gz')
            if not archive.exists():
                request = urllib.request.Request('https://codeload.github.com/' + repo_url + '/tar.gz/' + revision,
                                                 headers={'User-Agent': 'VintagePomelo-FEX-diagnostic'})
                with urllib.request.urlopen(request, timeout=45) as response:
                    data = response.read(32 * 1024 * 1024 + 1)
                if len(data) > 32 * 1024 * 1024:
                    raise ValueError('Dependency download size limit')
                archive.write_bytes(data)
            if sha(archive) != expected:
                raise ValueError('Dependency archive checksum mismatch')
            extract(archive.read_bytes(), destination, strip=True)
    lock_path.write_text(json.dumps(lock, indent=2) + '\n')
    patch = ROOT / 'scripts/patches/fex-valve-2607-pop-memory-precise-stack.patch'
    for name in PATCH_FILES:
        (source / name).write_bytes(git(repository, 'show', REVISION + ':' + name))
    if not args.stock_control:
        subprocess.run(['patch', '-p1', '--fuzz=0', '--batch', '-i', str(patch)], cwd=source, check=True)
    upstream_patch = ROOT / 'scripts/patches/fex-upstream-5901-restore-sigreturn-state.patch'
    upstream_sha = 'de76b76140e2933a574ea5427b372aa2512980c694a9aa37f9aac8f37708ee3e'
    if args.upstream_sigreturn:
        if sha(upstream_patch) != upstream_sha:
            raise ValueError('Published upstream patch checksum mismatch')
        subprocess.run(['patch', '-p1', '--fuzz=0', '--batch', '-i', str(upstream_patch)], cwd=source, check=True)
    version = ('FEX-2607-g37265b1-vpp-' + ('sigret' if args.stock_control else 'pop-sr')) if args.upstream_sigreturn else ('FEX-2607-76-g37265b1-vpp-' + ('stock' if args.stock_control else 'pop'))
    flags = (f'--target=aarch64-none-linux-gnu --gcc-toolchain={gnu} '
             f'--sysroot={gnu}/aarch64-none-linux-gnu/libc -isystem {uapi} '
             f'-isystem {uapi}/aarch64-linux-gnu')
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
        '-DBUILD_STEAM_SUPPORT=ON', '-DBUILD_TESTING=OFF', '-DBUILD_THUNKS=OFF', '-DENABLE_LTO=OFF',
        '-DENABLE_LIBCXX=OFF', '-DENABLE_GDB_SYMBOLS=OFF', '-DENABLE_JEMALLOC_GLIBC_ALLOC=ON',
        '-DENABLE_FEX_ALLOCATOR=ON', '-DENABLE_OFFLINE_TELEMETRY=OFF', '-DENABLE_VIXL_DISASSEMBLER=OFF',
        '-DENABLE_ZYDIS=OFF', '-DTUNE_CPU=generic',
        '-DOVERRIDE_VERSION=' + version,
        '-DOVERRIDE_HASH=' + REVISION]
    inputs = dict(source=lock, patchSha256=None if args.stock_control else sha(patch),
                  compilerSha256=sha(clang), toolchainSha256=sha(toolchain), options=options,
                  upstreamSigreturnPatchSha256=upstream_sha if args.upstream_sigreturn else None)
    manifest = output / ('sigreturn-manifest.json' if args.upstream_sigreturn else 'stock-manifest.json' if args.stock_control else 'manifest.json')
    old = json.loads(manifest.read_text()) if manifest.exists() else {}
    binary = output / 'build/Bin/FEX'
    if old.get('inputs') == inputs and binary.is_file() and old['executables']['FEX']['sha256'] == sha(binary):
        print('Reused matching FEX build; SHA-256 verified')
        return
    commands = [('configure', ['cmake', '-S', source, '-B', output / 'build', '-G', 'Ninja',
                                '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), *options]),
                ('build', ['cmake', '--build', output / 'build', '--target', 'FEX', 'FEXServer', '--parallel', str(args.jobs)])]
    for name, command in commands:
        with (output / (name + '.log')).open('w') as log:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((output / (name + '.log')).read_text()[-8000:])
            raise SystemExit(result.returncode)
    record = dict(source=lock, patchSha256=inputs['patchSha256'],
                  upstreamSigreturnPatchSha256=inputs['upstreamSigreturnPatchSha256'], inputs=inputs, executables={})
    for name in ['FEX', 'FEXServer']:
        path = output / 'build/Bin' / name
        record['executables'][name] = dict(bytes=path.stat().st_size, sha256=sha(path))
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record['executables'], indent=2))


if __name__ == '__main__':
    main()
