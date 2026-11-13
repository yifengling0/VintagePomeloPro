#!/usr/bin/env python3
"""Launch the external native ARM64 Steam research session through HDC.

Desktop uses a real Wine HWND to host rootful Xwayland inside the existing desktop.
Fullscreen/windowed remain explicit standalone diagnostics. GNU Steam stays native.
Never stops an application, clears its data, installs a HAP or sends input.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import tempfile


BUNDLE = "com.vintage.pomelopro"
CONFIG = "/data/storage/el2/base/files/steamohos-native/client-launch.conf"


def launch_plan(presentation, run_id, steamclient_probe=False, steamclient_rpc_probe=False, timeout_ms=14400000, steamclient_source_probe=False, host_window="task", big_picture=False, cef_gpu=False, graphics_self_test=False):
    if not 1000 <= timeout_ms <= 14400000:
        raise ValueError("timeout must be 1000-14400000 milliseconds")
    geometry = "1280x800"
    config = "\n".join([
        "timeoutMs=" + str(timeout_ms), "arg=-cef-disable-sandbox",
        "mesaDriver=virpipe", "socketMetadata=1",
        "desktopGeometry=" + geometry, "",
    ])
    if not cef_gpu:
        config += "arg=-cef-disable-gpu\narg=-cef-disable-gpu-compositing\n"
    else:
        config += "arg=-cef-verbose-logging\n"
    if graphics_self_test:
        config += "graphicsSelfTest=1\n"
    if big_picture:
        config += "arg=-gamepadui\n"
    if presentation == "fullscreen":
        config += "desktopFullscreen=1\n"
    if presentation == "desktop":
        config += "wineWindowHost=1\n"
    if steamclient_source_probe:
        config += "steamClientProbe=source-identity\n"
    elif steamclient_probe or steamclient_rpc_probe:
        config += "steamClientProbe=" + ("read-only-rpc" if steamclient_rpc_probe else "read-only") + "\n"
    arguments = ["shell", "aa", "start", "-b", BUNDLE, "-a", "EntryAbility",
                 "--ps", "winehua.mode", "smoke", "--ps", "winehua.suite", "linux-steam-client",
                 "--ps", "winehua.run_id", run_id]
    if presentation != "existing":
        arguments += ["--ps", "winehua.desktopMode", "fusion" if presentation in ("windowed", "fullscreen") else "virtual"]
    if presentation in ("windowed", "fullscreen"):
        arguments += ["--ps", "winehua.fusionHost", "ability" if host_window == "task" else "subwindow"]
    return {
        "presentation": presentation, "hostWindow": host_window, "bigPicture": big_picture,
        "requiresColdHost": presentation != "existing",
        "windowedStatus": "separate host candidate; not integrated X11 window management",
        "displayHost": "managed_subwindow" if presentation == "windowed" else ("current_policy" if presentation == "existing" else "standalone_fullscreen" if presentation == "fullscreen" else "shared_wine_desktop"),
        "steamGameLaunchVerified": False,
        "sharedDesktopCoexistenceVerified": False,
        "steamClientReadOnlyProbeRequested": steamclient_probe or steamclient_rpc_probe or steamclient_source_probe,
        "steamClientSourceProbeRequested": steamclient_source_probe,
        "steamClientRpcProbeRequested": steamclient_rpc_probe,
        "config": config, "arguments": arguments,
        "timeoutMs": timeout_ms, "cefGpuRequested": cef_gpu, "cefGpuEnabled": False,
        "graphicsSelfTestRequested": graphics_self_test,
        "runtimeLocation": "external Z: SteamLinuxARM64; original steamrtarm64/steam",
    }


def host_processes(output):
    # ps -A -o PID,PPID,NAME contains names only, never arguments/account data.
    rows = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            if fields[2] == BUNDLE or fields[2].startswith(BUNDLE + ":"):
                rows.append((int(fields[0]), fields[2]))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hdc", default="hdc")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--target")
    target.add_argument("--target-file", type=Path)
    parser.add_argument("--presentation", choices=["desktop", "existing", "windowed", "fullscreen"], default="desktop")
    parser.add_argument("--host-window", choices=["task", "subwindow"], default="task", help="Independent system task or legacy child window")
    parser.add_argument("--big-picture", action="store_true", help="Request the original native Steam gamepad UI")
    parser.add_argument("--cef-gpu", action="store_true", help="Opt-in CEF GPU diagnostic; renderer verification is still required")
    parser.add_argument("--graphics-self-test", action="store_true", help="Run visible Xlib/GLX readback fixtures before Steam (diagnostics only)")
    parser.add_argument("--timeout-ms", type=int, default=14400000,
                        help="Bounded research lifetime, 1000-14400000 milliseconds")
    parser.add_argument("--steamclient-source-probe", action="store_true",
                        help="Fresh pre/post negative controls plus native ELF and kernel socket peer verification; stops only its own Linux Steam test process after 100 seconds")
    parser.add_argument("--steamclient-rpc-probe", action="store_true",
                        help="Also expose the bounded, authenticated read-only PE diagnostic transport")
    parser.add_argument("--steamclient-probe", action="store_true",
                        help="Run the read-only GNU Steam helper; requires the matching host/runtime candidate")
    parser.add_argument("--run-id", default="steam-arm64-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    parser.add_argument("--artifact-dir", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="Print configuration/Want without contacting any device")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", args.run_id):
        parser.error("run-id must be 1-80 ASCII letters, digits, underscores or hyphens")
    if not 1000 <= args.timeout_ms <= 14400000:
        parser.error("timeout-ms must be between 1000 and 14400000")
    if args.steamclient_source_probe and (args.steamclient_probe or args.steamclient_rpc_probe):
        parser.error("source isolation and RPC probes use separate test runs")
    if args.steamclient_source_probe and args.timeout_ms < 130000:
        parser.error("source isolation requires at least 130000 milliseconds for controls and cleanup")
    plan = launch_plan(args.presentation, args.run_id, args.steamclient_probe, args.steamclient_rpc_probe, args.timeout_ms, args.steamclient_source_probe, args.host_window, args.big_picture, args.cef_gpu, args.graphics_self_test)
    if args.dry_run:
        print(json.dumps(plan, indent=2))
        return 0
    selected = args.target_file.read_text(encoding="utf-8").strip() if args.target_file else args.target
    if not selected or any(c.isspace() for c in selected):
        parser.error("provide one explicit target or a target-file; automatic device selection is not used")
    if args.artifact_dir is None:
        parser.error("artifact-dir is required to record configuration backup and operations")
    artifact = args.artifact_dir.resolve()
    artifact.mkdir(parents=True, exist_ok=True)
    ledger_path = artifact / (args.run_id + "-launch.json")
    if ledger_path.exists():
        parser.error("run-id already has a launch record; choose a new run-id")
    ledger = {"plan": plan, "device": "explicit target (identifier omitted)", "operations": []}

    def save(status):
        ledger["status"] = status
        ledger_path.write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")

    def command(arguments):
        result = subprocess.run([args.hdc, "-t", selected, *arguments], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        ledger["operations"].append({"arguments": arguments, "exitCode": result.returncode})
        save("in_progress")
        if result.returncode or "[Fail]" in result.stdout or "[Fail]" in result.stderr:
            raise RuntimeError("HDC operation failed; see operation/exit status in " + str(ledger_path))
        return result.stdout

    try:
        rows = host_processes(command(["shell", "ps", "-A", "-o", "PID,PPID,NAME"]))
        if any("steamohos_gnu" in name for _, name in rows):
            save("blocked_native_session_running")
            print("Steam/GNU session is already running; restore its current desktop instead of starting a second copy.")
            return 2
        if plan["requiresColdHost"] and rows:
            save("blocked_host_running")
            print("Presentation takes effect on a cold host launch. Exit the current game/application first; nothing was stopped or changed.")
            return 2
        backup = artifact / (args.run_id + "-previous-client-launch.conf")
        command(["file", "recv", "-b", BUNDLE, CONFIG, str(backup)])
        if not backup.is_file() or backup.stat().st_size > 4096:
            raise RuntimeError("Cannot back up the existing bounded launch configuration; prepare the external runtime first")
        with tempfile.TemporaryDirectory(prefix="steam-arm64-launch-") as temporary:
            config = Path(temporary) / "client-launch.conf"
            config.write_text(plan["config"], encoding="utf-8")
            try:
                command(["file", "send", "-b", BUNDLE, str(config), CONFIG])
                output = command(plan["arguments"])
                if "start ability successfully" not in output.lower():
                    raise RuntimeError("The system did not confirm the launch request")
            except Exception:
                command(["file", "send", "-b", BUNDLE, str(backup), CONFIG])
                raise
        save("launch_requested_not_startup_verified")
        print("Native ARM64 Steam launch requested (bounded research session, CEF GPU " + ("requested; unverified" if args.cef_gpu else "disabled") + ").")
        print("Desktop hosts the native Xwayland content inside a Wine HWND; Steam controls its presentation." if args.presentation == "desktop" else
              "Standalone diagnostic presentation selected; this mode does not use the Wine HWND bridge.")
        return 0
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        save("launch_failed")
        print(str(error).replace(selected, "<selected-device>"))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
