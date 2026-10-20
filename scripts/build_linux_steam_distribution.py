#!/usr/bin/env python3
"""Build a pinned downloader plan; Valve bytes stay on Valve's CDN.

The project archive contains only the GNU runtime/adapters and a CA bundle.
Every official client file is mapped back to a checksum-verified official ZIP.
No account, game, save or machine-specific configuration is accepted as input.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import posixpath
import shutil
import zipfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode()


def safe(value):
    path = PurePosixPath(value)
    if (not value or value.startswith("/") or "\\" in value or ":" in value or
            any(ord(c) < 32 for c in value) or any(p in ("", ".", "..") for p in value.split("/"))):
        raise ValueError("Unsafe relative path: " + value)
    return str(path)


def build(runtime, package_lock, downloads, output, project_url):
    manifest = json.loads((runtime / "manifest.json").read_text(encoding="utf-8"))
    packages = json.loads(package_lock.read_text(encoding="utf-8"))["packages"]
    helper = manifest["steamClientProbe"]["helpers"]["provenance"]
    helper_path = downloads / helper["package"]
    helper_bytes = helper_path.read_bytes()
    if hashlib.sha1(helper_bytes).hexdigest() != helper["packageSha1"]:
        raise ValueError("Official helper package identity mismatch")
    packages["bins_misc_linuxarm64"] = {"file": helper["package"], "size": len(helper_bytes),
                                        "sha2": digest(helper_bytes)}
    names = {"bins_linuxarm64_linuxarm64": "Steam ARM64 客户端",
             "webkit_linuxarm64_linuxarm64": "Steam 网页运行组件",
             "sdl3_linuxarm64_linuxarm64": "音频与手柄组件",
             "steamui_websrc_all": "Steam 界面资源", "bins_misc_linuxarm64": "游戏启动辅助组件"}
    index, archives = {}, []
    for key, record in packages.items():
        path = downloads / record["file"]
        if path.name != record["file"] or path.stat().st_size != int(record["size"]):
            raise ValueError("Official archive size mismatch: " + key)
        if digest(path.read_bytes()) != record["sha2"]:
            raise ValueError("Official archive checksum mismatch: " + key)
        total, seen, links = 0, set(), {}
        with zipfile.ZipFile(path) as source:
            for member in source.infolist():
                name = safe(member.filename.replace("\\", "/").rstrip("/"))
                if name in seen:
                    raise ValueError("Duplicate ZIP member: " + name)
                seen.add(name)
                if member.is_dir():
                    continue
                total += member.file_size
                # Official links are not used as a source of regular data.
                if stat.S_ISLNK(member.external_attr >> 16):
                    target = source.read(member).decode('utf-8')
                    resolved = safe(posixpath.normpath(posixpath.join(posixpath.dirname(name), target)))
                    if target.startswith('/') or '\\' in target or ':' in target:
                        raise ValueError('Unsafe ZIP link: ' + name)
                    links[name] = resolved
                    continue
                content_hash = digest(source.read(member))
                index.setdefault(content_hash, (key, name, member.filename))
            for name in seen:
                if any(str(parent) in links for parent in PurePosixPath(name).parents):
                    raise ValueError('ZIP writes through a link: ' + name)
            for name, target in links.items():
                if target not in seen or target in links or any(str(parent) in links for parent in PurePosixPath(target).parents):
                    raise ValueError('ZIP link target is not a materialized member: ' + name)
        archives.append({"id": key, "name": names.get(key, "Steam 资源 · " + key.split("_all")[0]),
                         "url": "https://client-update.steamstatic.com/" + record["file"],
                         "bytes": int(record["size"]), "sha256": record["sha2"], "unpackedBytes": total})
    output.mkdir(parents=True, exist_ok=True)
    project_files, mappings, release_files, client_files = {}, [], [], []

    def add(target, data):
        safe(target)
        sha = digest(data)
        if sha in index:
            archive, source, zip_path = index[sha]
        else:
            # Proprietary Steam code/data must never leak into the project ZIP.
            allowed = target.startswith("release/runtime/") and "/steamrtarm64/" not in target
            allowed = allowed or target == "client/steamohos/ca-certificates.crt"
            if not allowed:
                raise ValueError("Unmapped official client file: " + target)
            archive, source, zip_path = "gnu-runtime", target, target
            project_files[source] = data
        mappings.append({"path": target, "archive": archive, "source": source, "zipPath": zip_path,
                         "bytes": len(data), "sha256": sha})
        record = {"path": target.split("/", 1)[1], "bytes": len(data), "sha256": sha}
        (release_files if target.startswith("release/") else client_files).append(record)

    for name in ("steam-client-data.zip", "steam-client-helpers.zip"):
        with zipfile.ZipFile(runtime / name) as source:
            for member in source.infolist():
                if not member.is_dir():
                    add("client/" + safe(member.filename), source.read(member))
    client_files.sort(key=lambda item: item["path"])
    client_hash = digest(encode(client_files))
    essential_names = {"steamui/index.html", "steamrtarm64/icudtl.dat", "bin/d3ddriverquery64.exe",
                       "legacycompat/iscriptevaluator.exe", "steamohos/ca-certificates.crt"}
    essential = [item for item in client_files if item["path"] in essential_names]
    if len(essential) != len(essential_names):
        raise ValueError("Essential client resources missing")
    manifest["steamClientProbe"]["data"] = {"kind": "installed-resources", "sha256": client_hash,
                                          "essential": essential}
    manifest["steamClientProbe"].pop("helpers", None)
    # Source paths are metadata, never user data. Retain provenance/licenses.
    manifest_bytes = encode(manifest)
    omitted = {"manifest.json", "steam-client-data.zip", "steam-client-helpers.zip"}
    for path in sorted(runtime.rglob("*")):
        if path.is_symlink():
            raise ValueError("Runtime input must have materialized regular files: " + str(path))
        if path.is_file() and path.relative_to(runtime).as_posix() not in omitted:
            add("release/runtime/" + path.relative_to(runtime).as_posix(), path.read_bytes())
    add("release/runtime/manifest.json", manifest_bytes)
    release_files.sort(key=lambda item: item["path"])
    steam = next(item for item in release_files if item["path"] == "runtime/steamrtarm64/steam")
    release = {"schemaVersion": 1, "layout": "SteamLinuxARM64", "executionModel": "native_aarch64",
               "steamSha256": steam["sha256"], "files": release_files}
    release_text = encode(release).decode()
    archive_path = output / "hmosdeck-gnu-arm64-v1.zip"
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as destination:
        for name, data in sorted(project_files.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            destination.writestr(entry, data)
    archives.insert(0, {"id": "gnu-runtime", "name": "HmosDeck GNU 运行组件", "url": project_url,
                        "bytes": archive_path.stat().st_size, "sha256": digest(archive_path.read_bytes()),
                        "unpackedBytes": sum(map(len, project_files.values()))})
    cached = sum(item["bytes"] for item in archives)
    total = sum(item["bytes"] for item in archives if item["url"].startswith("https://"))
    unpacked = sum(item["bytes"] for item in mappings)
    plan = {"schemaVersion": 1, "hostAbi": 1, "id": digest(release_text.encode()),
            "clientBuild": json.loads(package_lock.read_text(encoding="utf-8"))["version"],
            "clientDataId": client_hash, "downloadBytes": total,
            "requiredSpaceBytes": cached + unpacked * 2 + max(a["unpackedBytes"] for a in archives) + 536870912,
            "archives": archives, "files": mappings, "releaseManifest": release_text}
    (output / "deployment.json").write_bytes(encode(plan))
    summary = {"id": plan["id"], "downloadBytes": total, "requiredSpaceBytes": plan["requiredSpaceBytes"],
               "projectArchiveBytes": archive_path.stat().st_size, "projectFiles": len(project_files),
               "officialArchives": len(archives) - 1, "mappedFiles": len(mappings),
               "deploymentSha256": digest(encode(plan)), "containsValvePayload": False}
    (output / "distribution-summary.json").write_bytes(encode(summary))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "package-lock", "downloads", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--project-url", required=True)
    parser.add_argument("--rawfile-dir", type=Path,
                        help="Also install deployment.json and the GNU ZIP into this generated rawfile directory")
    args = parser.parse_args()
    result = build(args.runtime, args.package_lock, args.downloads, args.output, args.project_url)
    if args.rawfile_dir:
        if args.project_url != 'rawfile://linux-steam/hmosdeck-gnu-arm64-v1.zip':
            raise ValueError('Rawfile installation requires the rawfile URL')
        args.rawfile_dir.mkdir(parents=True, exist_ok=True)
        for name in ('deployment.json', 'hmosdeck-gnu-arm64-v1.zip'):
            destination = args.rawfile_dir / name
            source = args.output / name
            if not destination.exists() or digest(destination.read_bytes()) != digest(source.read_bytes()):
                shutil.copyfile(source, destination)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
