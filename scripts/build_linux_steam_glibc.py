#!/usr/bin/env python3
"""Build the paired native AArch64 GNU runtime with the OHOS ELF mapping patch."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARCHIVE_SHA256 = "fb82998998b2b29965467bc1b69d152e9c307d2cf301c9eafb4555b770ef3fd2"
PATCHES = [ROOT / "patches/glibc" / name for name in [
    "0001-ohos-map-load-segments-within-file.patch",
    "0002-ohos-use-legacy-clone-for-native-threads.patch",
    "0004-ohos-spawn-with-legacy-clone.patch",
    "0005-ohos-query-current-native-robust-list.patch",
    "0007-ohos-use-native-fstat-for-aarch64-descriptors.patch"]]


def output(command):
    return subprocess.check_output([str(x) for x in command], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--external-native-memory", action="store_true",
                        help="Allow opt-in native memory images from a verified external runtime root")
    args = parser.parse_args()
    patches = PATCHES + ([ROOT / "patches/glibc/0003-ohos-opt-in-external-native-memory.patch",
                          ROOT / "patches/glibc/0006-ohos-protect-external-bti-in-place.patch",
                          ROOT / "patches/glibc/0008-ohos-native-map-failure-details.patch",
                          ROOT / "patches/glibc/0009-ohos-enable-external-executable-memory.patch"]
                          if args.external_native_memory else [])
    if args.output.exists() or not 1 <= args.jobs <= 32:
        parser.error("Use a new output directory and 1–32 jobs.")
    if hashlib.sha256(args.archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        parser.error("Expected the pinned GNU glibc-2.38.tar.xz archive.")
    cc = pathlib.Path(shutil.which(args.cc) or args.cc).resolve()
    target = output([cc, "-dumpmachine"])
    if not target.startswith("aarch64") or "linux-gnu" not in target:
        parser.error("A genuine GNU/Linux AArch64 compiler is required.")
    task_output = args.output.resolve()
    task_output.mkdir(parents=True)
    subprocess.run(["tar", "-xf", str(args.archive.resolve()), "-C", str(task_output), "--no-same-owner"], check=True)
    source = task_output / "glibc-2.38"
    for path in patches:
        with path.open("rb") as patch:
            subprocess.run(["patch", "-p1", "--fuzz=0", "--forward", "--batch"], cwd=source, stdin=patch, check=True)
    build = task_output / "build"
    build.mkdir()
    sysroot = pathlib.Path(output([cc, "-print-sysroot"]))
    environment = os.environ.copy()
    environment.update(CC=str(cc), CXX=str(cc).removesuffix("gcc") + "g++",
                       PATH=str(cc.parent) + os.pathsep + environment["PATH"])
    configure = [str(source / "configure"), "--prefix=/usr", "--host=" + target,
                 "--build=" + output([source / "scripts/config.guess"]),
                 "--with-headers=" + str(sysroot / "usr/include"), "--disable-werror",
                 "--disable-nscd", "--enable-kernel=3.7.0"]
    for name, command in [("configure", configure), ("build", ["make", "-j" + str(args.jobs)])]:
        with (task_output / (name + ".log")).open("w") as log:
            subprocess.run(command, cwd=build, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
    manifest = {"version": "2.38", "sourceUrl": "https://ftp.gnu.org/gnu/glibc/glibc-2.38.tar.xz",
                "archiveSha256": ARCHIVE_SHA256,
                "patches": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in patches},
                "compiler": str(cc), "target": target, "configure": configure,
                "files": {name: hashlib.sha256((build / name).read_bytes()).hexdigest()
                          for name in ["elf/ld.so", "libc.so"]}}
    (task_output / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"build": str(task_output), "target": target, "version": "2.38"}))


if __name__ == "__main__":
    main()
