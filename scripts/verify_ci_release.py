#!/usr/bin/env python3
"""Fail closed on wrong source/tag/gitlinks or mismatched packaged versions."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_FIELDS = ("bundleName", "versionName", "versionCode")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def load_app(root):
    # AppScope is intentionally JSON-compatible (no general JSON5 parser needed).
    return json.loads((root / "AppScope/app.json5").read_text(encoding="utf-8"))["app"]


def verify_source(root, expected_sha, release_tag):
    sha = git(root, "rev-parse", "HEAD")
    if sha != expected_sha:
        raise ValueError(f"checkout SHA mismatch: expected {expected_sha}, got {sha}")
    app = load_app(root)
    if release_tag:
        if not re.fullmatch(r"(?:rc|dev)-[0-9A-Za-z][0-9A-Za-z._-]*", release_tag):
            raise ValueError("invalid release tag")
        try:
            tag_sha = git(root, "rev-parse", f"refs/tags/{release_tag}^{{commit}}")
        except subprocess.CalledProcessError as error:
            raise ValueError(f"release tag not in checkout: {release_tag}") from error
        if tag_sha != sha:
            raise ValueError("release tag points to a different commit")
        if release_tag.startswith("rc-") and release_tag != "rc-" + app["versionName"]:
            raise ValueError("RC tag does not match AppScope versionName")

    submodules = {}

    def visit(repo, prefix=""):
        # Read committed trees, not .gitmodules branch tips or a modified index.
        subprocess.run(["git", "-C", str(repo), "diff", "--quiet", "HEAD",
                        "--ignore-submodules=all"], check=True)
        for record in git(repo, "ls-tree", "-rz", "HEAD").split("\0"):
            if not record:
                continue
            meta, path = record.split("\t", 1)
            mode, kind, pinned = meta.split()
            if mode != "160000":
                continue
            child = repo / path
            label = prefix + path
            if not (child / ".git").exists():
                raise ValueError(f"submodule not initialized: {label}")
            actual = git(child, "rev-parse", "HEAD")
            if actual != pinned:
                raise ValueError(f"gitlink mismatch: {label}: {actual} != {pinned}")
            submodules[label] = pinned
            visit(child, label + "/")

    visit(root)
    patches = {str(p.relative_to(root)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted((root / "patches").rglob("*.patch"))}
    return {"commit": sha, "releaseTag": release_tag,
            "app": {key: app[key] for key in APP_FIELDS},
            "submodules": submodules, "patches": patches}


def verify_artifact(hap, manifest):
    with zipfile.ZipFile(hap) as archive:
        app = json.loads(archive.read("module.json"))["app"]
        for key in APP_FIELDS:
            if app.get(key) != manifest["app"][key]:
                raise ValueError(f"HAP {key} differs from checked-out source")
        for key in ("minAPIVersion", "targetAPIVersion"):
            # OHOS metadata may encode 6.1.0(23) as 60100023 or as 23.
            if app.get(key) not in (23, 60100023):
                raise ValueError(f"unexpected HAP {key}: {app.get(key)}")
        names = archive.namelist()
        abis = {name.split("/")[1] for name in names if name.startswith("libs/")
                and len(name.split("/")) > 2}
        if abis != {"arm64-v8a"}:
            raise ValueError(f"unexpected native ABIs: {sorted(abis)}")
        for library in ("box64.so", "libentry.so", "libwine_child.so",
                        "libwinehua_vtest_server.so", "libvirglrenderer.so.1", "libvirgl_child.so"):
            with archive.open("libs/arm64-v8a/" + library) as stream:
                header = stream.read(20)
            if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
                raise ValueError(f"not an AArch64 ELF: {library}")
        runtime = archive.getinfo("resources/rawfile/wine-data.zip")
        if runtime.file_size == 0:
            raise ValueError("empty guest runtime")
    digest = hashlib.sha256()
    with hap.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"commit": manifest["commit"], "releaseTag": manifest["releaseTag"],
            "app": manifest["app"], "abi": "arm64-v8a", "api": 23,
            "file": hap.name, "size": hap.stat().st_size, "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    source = modes.add_parser("source")
    source.add_argument("--expected-sha", required=True)
    source.add_argument("--release-tag", default="")
    source.add_argument("--output", type=Path, required=True)
    artifact = modes.add_parser("artifact")
    artifact.add_argument("--manifest", type=Path, required=True)
    artifact.add_argument("--hap", type=Path, required=True)
    artifact.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "source":
        result = verify_source(ROOT, args.expected_sha, args.release_tag)
    else:
        result = verify_artifact(args.hap, json.loads(args.manifest.read_text(encoding="utf-8")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"CI version gate: {error}", file=sys.stderr)
        sys.exit(1)
