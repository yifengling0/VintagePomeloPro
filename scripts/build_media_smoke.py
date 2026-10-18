#!/usr/bin/env python3
"""Build video playback probes and synthetic fixtures; never touches user games.

Copy the output to C:\\smoke, then run automation/smoke.py with --inline job.json.
Each combination must complete twice with both video and audio selected.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mingw", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arch", choices=("x86_64", "i686", "aarch64"), default="x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    executable_dir = "x86" if args.arch == "i686" else "x64" if args.arch == "aarch64" else "x64-fex"
    exe = executable_dir + "/media_" + args.arch + ".exe"
    (args.out / executable_dir).mkdir(exist_ok=True)
    (args.out / "assets/media").mkdir(parents=True, exist_ok=True)
    subprocess.run([str(args.llvm_mingw / "bin" / (args.arch + "-w64-mingw32-clang++")),
                    "-O2", "-Wall", "-Wextra", "-static", "-municode",
                    str(ROOT / "smoke/winehua_media_smoke.cpp"), "-o", str(args.out / exe),
                    "-lole32", "-loleaut32", "-lstrmiids", "-lmfplay", "-lmfuuid", "-luuid", "-luser32", "-lgdi32"], check=True)
    formats = {"h264-aac.mp4": ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "ultrafast", "-c:a", "aac", "-movflags", "+faststart"],
               "wmv-asf.wmv": ["-c:v", "wmv2", "-c:a", "wmav2"]}
    for name, options in formats.items():
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24",
                        "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=44100",
                        "-t", "4", *options, str(args.out / "assets/media" / name)], check=True)
    tests = []
    for route in ("directshow", "mf"):
        for name in formats:
            tests.append({"testId": route + "-" + name.split(".")[0], "exe": exe,
                          "extraArgs": ["--route", route, "--file", "C:\\smoke\\assets\\media\\" + name, "--repeat", "2"],
                          "timeoutMs": 90000, "d3dBackend": "wined3d"})
    (args.out / "job.json").write_text(json.dumps({"tests": tests}, indent=2) + "\n")
    files = {str(path.relative_to(args.out)): hashlib.sha256(path.read_bytes()).hexdigest()
             for folder in (executable_dir, "assets") for path in sorted((args.out / folder).rglob("*")) if path.is_file()}
    suites = {"schemaVersion": 1, "suites": {"video-playback": {"tests": tests}}}
    version = "smoke-v2-" + hashlib.sha256(json.dumps({"suites": suites, "files": files}, sort_keys=True).encode()).hexdigest()[:12]
    suites["suiteVersion"] = version
    (args.out / "suites.json").write_text(json.dumps(suites, indent=2) + "\n")
    (args.out / "manifest.json").write_text(json.dumps({"schemaVersion": 1, "suiteVersion": version, "files": files}, indent=2) + "\n")
    print(f"Media smoke ready: {args.out}; 4 combinations, 2 plays each")


if __name__ == "__main__":
    main()
