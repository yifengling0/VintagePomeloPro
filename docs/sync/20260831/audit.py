"""Read-only audit of this integration's frozen source and product boundaries."""
from pathlib import Path
import collections
import json
import re
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO, text=True).strip()


ledger = json.loads((HERE / "commits.json").read_text())
mapping = json.loads((HERE / "path-map.json").read_text())["paths"]
base, start, tip = (ledger[k] for k in ("product_base", "upstream_base", "upstream_tip"))
sources = git("rev-list", f"{start}..{tip}").splitlines()
recorded = [c["source"] for c in ledger["commits"]]
assert len(recorded) == len(set(recorded)) == len(sources)
assert set(recorded) == set(sources), "Source interval has missing/extra ledger entries"
allowed = {"adapted", "covered_by_product", "reference_only", "not_applicable", "superseded"}
for item in ledger["commits"]:
    assert item["status"] in allowed, item["source"]
    assert item.get("disposition"), item["source"]
    if item["status"] == "adapted":
        assert git("merge-base", "--is-ancestor", item["checkpoint"], "HEAD") == ""

protected = [
    "AppScope", ".gitmodules", "entry/src/main/module.json5", "entry/src/main/resources",
    "entry/src/main/ets/components",
    "entry/src/main/ets/entryability/DesktopAbility.ets",
    "entry/src/main/ets/entryability/WineWindowAbility.ets",
    *[f"entry/src/main/ets/service/{name}.ets" for name in (
        "GamepadManager", "InputDispatcher", "InputRouter")],
]
# Only the settings page and shared settings model are intentionally changed in
# this increment; every other product page remains protected.
protected.extend(path for path in git("ls-files", "entry/src/main/ets/pages").splitlines()
                 if path != "entry/src/main/ets/pages/SystemSettings.ets")
# Compare both committed and working-tree content: do not overlook an unstaged UI change.
changed = git("diff", "--name-only", base, "--", *protected)
assert not changed, f"Protected product paths changed:\n{changed}"


def gitlinks(ref):
    return {line.split("\t", 1)[1]: line.split()[2]
            for line in git("ls-tree", "-r", ref).splitlines() if line.startswith("160000 ")}


pins = gitlinks(base)
assert pins == gitlinks("HEAD"), "A product gitlink changed"
# Submodule worktrees may contain pre-existing local build/debug edits. The
# committed gitlink equality above is the boundary relevant to this audit;
# never clean or rewrite those worktrees here.
submodules = subprocess.check_output(["git", "submodule", "status", "--recursive"], cwd=REPO, text=True)
assert all(line.startswith(" ") for line in submodules.splitlines()), "Uninitialized or mismatched recursive pin"

cpp = "entry/src/main/cpp/"
body_base = "7494e63f"  # Semantic integration is complete; T7 only moves paths/includes.
relocation_tip = git("rev-parse", "f05cb825")
assert git("merge-base", "--is-ancestor", relocation_tip, "HEAD") == ""


def normalize(body):
    return re.sub(r"^\s*#\s*include[^\n]*(?:\n|$)", "", body, flags=re.M).strip()


for old, new in mapping.items():
    before = git("show", f"{body_base}:{cpp}{old}")
    # Prove T7 itself is mechanical even after separately reviewed runtime fixes.
    # Later Native changes are checked commit-by-commit below, never ignored.
    after = git("show", f"{relocation_tip}:{cpp}{new}")
    for previous, current in sorted(mapping.items(), key=lambda pair: len(pair[1]), reverse=True):
        # T7 changes both repository paths and relative include/path literals.
        after = after.replace(cpp + current, cpp + previous).replace(current, previous)
    assert normalize(before) == normalize(after), f"Non-mechanical relocation: {new}"

fixes = json.loads((HERE / "native-fixes.json").read_text())
actual_fixes = git("rev-list", "--reverse", f"{relocation_tip}..HEAD", "--", cpp).splitlines()
assert actual_fixes == [fix["commit"] for fix in fixes], "Unrecorded post-relocation Native commit"
for fix in fixes:
    changed_files = git("diff-tree", "--no-commit-id", "--name-only", "-r", fix["commit"], "--", cpp).splitlines()
    assert sorted(changed_files) == sorted(fix["native_paths"]), fix["commit"]
    assert fix["reason"] and fix["host_evidence"] and fix["device_status"]
assert not git("diff", "--name-only", "HEAD", "--", cpp), "Uncommitted Native edits need separate validation"
assert not git("ls-files", "--others", "--exclude-standard", "--", cpp), "Untracked Native source needs review"
upstream_native = set(git("ls-tree", "-r", "--name-only", tip, "--", cpp).splitlines())
head_native = set(git("ls-tree", "-r", "--name-only", "HEAD", "--", cpp).splitlines())
missing_native = sorted(upstream_native - head_native)
assert not missing_native, f"Missing upstream Native destinations: {missing_native}"

required_smoke = {
    "automation/run_regression.py", "automation/validate_frame.py",
    "entry/src/main/ets/common/SmokeTypes.ets",
    "entry/src/main/ets/service/SmokeRunner.ets",
    "smoke/winehua_gpu_diagnostics.c",
}
head_files = set(git("ls-tree", "-r", "--name-only", "HEAD").splitlines())
assert required_smoke <= head_files, f"Missing smoke architecture: {sorted(required_smoke - head_files)}"

contracts = {
    "entry/src/main/cpp/wine/wine_exe.h": ["dxvkBackend", "presentBackend"],
    "entry/src/main/cpp/wine/wine_launch.h": ["dxvkBackend", "wineLang"],
    "entry/src/main/cpp/wine/env_profiles.h": ["SessionEnvPolicy", "dxvkBackend", "wineLang"],
    "entry/src/main/cpp/bridge/napi_init.cpp": ["sixthType == napi_boolean", '"desktopShell"', "setHostShadowProfile", "gLegacyHostShadowProfile"],
    "entry/src/main/cpp/wine/wine_env.cpp": ['"LC_ALL=" + locale + ".UTF-8"'],
    "entry/src/main/ets/entryability/EntryAbility.ets": ["mode !== 'game' && mode !== 'smoke'", "ensureSmokeReady"],
    "entry/src/main/ets/service/WineEngineService.ets": ["activePrefixMode", "restoreProductSession"],
}
for path, fragments in contracts.items():
    body = git("show", f"HEAD:{path}")
    for fragment in fragments:
        assert fragment in body, f"Missing architecture contract {fragment!r} in {path}"
index_page = git("show", "HEAD:entry/src/main/ets/pages/Index.ets")
assert "SmokeRunner" not in index_page, "Upstream smoke UI leaked into product Index"

print(json.dumps({
    "status": "PASS", "head": git("rev-parse", "HEAD"),
    "source_commits": len(recorded), "dispositions": dict(collections.Counter(c["status"] for c in ledger["commits"])),
    "protected_product_paths": "unchanged", "top_level_gitlinks": len(pins),
    "recursive_pins": "matched", "mechanical_native_moves": len(mapping),
    "mechanical_checkpoint": relocation_tip, "post_relocation_native_fixes": [fix["commit"] for fix in fixes],
    "upstream_native_files": len(upstream_native),
    "upstream_native_destinations": "all present",
    "product_native_extra_files": len(head_native - upstream_native),
    "smoke_architecture": "adapted behind product UI",
    "common_native_control_plane": "compatible superset",
    "limit": "Source boundaries only; use separate build and device evidence for behavior."
}, indent=2))
