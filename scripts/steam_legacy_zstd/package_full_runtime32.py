"""Package the exact frozen Steam client with runtime32 and no user state.

Uses a positive list of shipped resource roots. Never copy a live installation
wholesale: config, userdata, logs, dumps, appcache, games and login files are not
distribution inputs. The original directory and helpers remain unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import zipfile

from package_runtime32 import package, digest


RESOURCE_ROOTS = (
    'bin', 'clientui', 'controller_base', 'friends', 'graphics', 'public',
    'resource', 'servers', 'steam/cached', 'steamui', 'steamui-public', 'tenfoot',
)
ROOT_FILES = '''1-Normal.bat 2-perf-gpu-auto.bat 3-perf-multiprocess.bat
crashhandler.dll crashhandler64.dll CSERHelper.dll d3dcompiler_46_64.dll
d3dcompiler_46.dll fossilize_engine_filters.json GameOverlayRenderer.dll
GameOverlayRenderer64.dll GameOverlayUI.exe GfnRuntimeSdk.dll icui18n.dll
icuuc.dll libavcodec-58.dll libavformat-58.dll libavresample-4.dll libavutil-56.dll
libfreetype-6.dll libharfbuzz-0.dll libswscale-5.dll libx264-142.dll.crypt
libx264-142.dll.md5 openvr_api.dll SDL3_ttf.dll SDL3.dll steam.cfg Steam.dll
steam.exe steam.signatures Steam2.dll steamclient.dll steamclient64.dll
steamerrorreporter.exe steamerrorreporter64.exe SteamFossilizeVulkanLayer.json
SteamFossilizeVulkanLayer64.json SteamOverlayVulkanLayer.dll
SteamOverlayVulkanLayer.json SteamOverlayVulkanLayer64.dll
SteamOverlayVulkanLayer64.json SteamUI.dll steamwebrtc.dll steamwebrtc64.dll
streaming_client.exe ThirdPartyLegalNotices.css ThirdPartyLegalNotices.doc
ThirdPartyLegalNotices.html tier0_s.dll tier0_s64.dll v8.dll video.dll
VkLayer_steam_fossilize.dll VkLayer_steam_fossilize64.dll vstdlib_s.dll
vstdlib_s64.dll WriteMiniDump.exe zlib1.dll'''.split()
PRIVATE_PARTS = {'userdata', 'config', 'appcache', 'logs', 'dumps', 'steamapps',
                 'htmlcache', 'shadercache', 'depotcache', 'avatarcache',
                 'gpu cache', 'gpucache', 'crashpad', 'loginusers.vdf', 'ssfn'}

README = '''旧柚 Pro 历史 Steam 完整包 · 下载兼容修复 · 2026-10-11

本包保留原来的 Steam / CEF 版本和 GPU 启动方式，不更新新版 Steam。
新增当前下载块 VSZa / Zstd 的解包支持，两个 steamclient DLL 保持原版，
避免旧补丁导致部分游戏（如仙剑奇侠传四）启动错误 51。
下载鉴权、解密及内容完整性校验保留。

使用方法
1. 完全退出 Steam 和旧柚 Pro 的 Wine 会话。
2. 将压缩包完整解压到一个新的游戏目录；不要只复制其中某个 DLL。
3. 在旧柚 Pro 内运行 Steam\\2-perf-gpu-auto.bat，登录自己的账号。
4. 本包不含账号、已下载游戏和个人存档。原目录请先保留，确认正常后再迁移游戏。
   不建议复制旧的浏览器缓存或混用不同补丁的 DLL。
5. 保留 steam.cfg，不要执行 Steam 自更新；新版客户端不属于本包适配范围。

已验证
- 旧柚 Pro 1.4.6 / 1004060，MatePad Mini 真机。
- Purrgatory 完整下载约 127 MB，56 个 VSZa 块成功，Steam 显示 Fully Installed / No Error。
- 关闭诊断后，Steam 版仙剑奇侠传四进入菜单，不再出现本轮复现的错误 51。
- 两个原版客户端 DLL 均有 SHA256 校验；64 位游戏实机启动/补下载尚未完成回归。

说明
这是针对特定历史客户端的兼容修改包，并非 Valve 官方发布包。
修复的是下载解包；游戏仍需满足旧柚 Pro 的图形等运行条件。
OpenGL 版本不足与下载成功是两个独立问题，本包不改变图形驱动能力。
本轮按需求不继续处理已有窗口稳定性问题，未作长时间稳定性保证。
遇到下载失败，可提供 logs\\content_log.txt 和 logs\\vpsteam-runtime32.log，
发送前请隐藏账号及 URL 中的鉴权参数。

SHA256SUMS.txt 记录包内发布文件的校验值。Zstd 许可见 VPP-ZSTD-LICENSE.txt。
'''


def allowed_resource(path):
    parts = {p.lower() for p in path.parts}
    return not (parts & PRIVATE_PARTS) and not any(
        p.lower().startswith('ssfn') for p in path.parts)


def build(original, helpers, output):
    original, helpers, output = original.resolve(), helpers.resolve(), output.resolve()
    if output.exists() or original in output.parents or helpers in output.parents:
        raise ValueError('Use a new output directory outside the input directories')
    selected = [original / n for n in ROOT_FILES]
    for name in RESOURCE_ROOTS:
        folder = original / name
        if not folder.is_dir():
            raise ValueError('Missing shipped resource directory: ' + name)
        selected.extend(p for p in folder.rglob('*') if p.is_file())
    selected = sorted(set(selected), key=lambda p: p.relative_to(original).as_posix())
    for p in selected:
        relative = p.relative_to(original)
        if not p.is_file() or p.is_symlink() or original not in p.resolve().parents:
            raise ValueError('Invalid distribution input: ' + str(relative))
        if not allowed_resource(relative):
            raise ValueError('Private state found in resource tree: ' + str(relative))
    # Validate every helper and original identity before writing distribution files.
    with tempfile.TemporaryDirectory(prefix='vpp-steam-overlay-') as scratch:
        overlay = Path(scratch) / 'overlay'
        tested = package(original, helpers, overlay)
        output.mkdir(parents=True)
        staging = output / 'Steam'
        staging.mkdir()
        source_hashes = {}
        for p in selected:
            relative = p.relative_to(original)
            dest = staging / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, dest)
            source_hashes[relative.as_posix()] = digest(p)
            if digest(dest) != source_hashes[relative.as_posix()]:
                raise ValueError('Source changed during copy: ' + str(relative))
        for name in [*tested['sha256'], 'VPP-ZSTD-LICENSE.txt']:
            shutil.copyfile(overlay / name, staging / name)
    (staging / '使用说明.txt').write_text(README, encoding='utf-8-sig')
    files = sorted(p for p in staging.rglob('*') if p.is_file())
    checksums = {p.relative_to(staging).as_posix(): digest(p) for p in files}
    (staging / 'SHA256SUMS.txt').write_text(''.join(
        f'{sha}  {name}\n' for name, sha in checksums.items()), encoding='utf-8')
    files = sorted(p for p in staging.rglob('*') if p.is_file())
    archive = output / 'Steam-legacy-download-fix-20261011.zip'
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in files:
            info = zipfile.ZipInfo('Steam/' + p.relative_to(staging).as_posix(),
                                   date_time=(2026, 10, 11, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            with p.open('rb') as source, z.open(info, 'w') as dest:
                shutil.copyfileobj(source, dest, 1024 * 1024)
    # Check every uncompressed member against the staged files, including README
    # and checksum manifest. This also checks ZIP CRC and rejects duplicates.
    with zipfile.ZipFile(archive) as z:
        if len(z.namelist()) != len(set(n.casefold() for n in z.namelist())):
            raise ValueError('Duplicate archive members')
        for p in files:
            name = 'Steam/' + p.relative_to(staging).as_posix()
            with z.open(name) as member:
                if hashlib.file_digest(member, 'sha256').hexdigest() != digest(p):
                    raise ValueError('ZIP verification failed: ' + name)
    unchanged = all(digest(original / n) == sha for n, sha in source_hashes.items())
    if not unchanged:
        raise ValueError('Original inputs changed during packaging')
    manifest = dict(name=archive.name, bytes=archive.stat().st_size,
                    sha256=digest(archive), files=len(files),
                    unpackedBytes=sum(p.stat().st_size for p in files),
                    originalInputsUnchanged=True, allZipMembersVerified=True,
                    excludedUserState=['config', 'userdata', 'appcache', 'logs',
                        'dumps', 'steamapps', 'steam/games', 'music', 'ssfn*',
                        '.cef-dev-tools-size.vdf', 'update_hosts_cached.vdf'],
                    runtimeValidation=tested,
                    filesSha256={p.relative_to(staging).as_posix():digest(p) for p in files})
    archive.with_suffix('.manifest.json').write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    archive.with_suffix('.sha256').write_text(
        f'{manifest["sha256"]}  {archive.name}\n', encoding='ascii')
    shutil.copyfile(staging/'使用说明.txt', output/'使用说明.txt')
    print(json.dumps({k:v for k,v in manifest.items() if k != 'filesSha256'}, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('original', type=Path)
    p.add_argument('helpers', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    build(a.original, a.helpers, a.output)
