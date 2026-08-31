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
    "entry/src/main/ets/components", "entry/src/main/ets/pages", "entry/src/main/ets/entryability",
    "entry/src/main/ets/model/AppModels.ets", "entry/src/main/cpp/types",
    *[f"entry/src/main/ets/service/{name}.ets" for name in (
        "GamepadManager", "InputDispatcher", "InputRouter")],
]
# Compare both committed and working-tree content: do not overlook an unstaged UI change.
changed = git("diff", "--name-only", base, "--", *protected)
assert not changed, f"Protected product paths changed:\n{changed}"


def gitlinks(ref):
    return {line.split("\t", 1)[1]: line.split()[2]
            for line in git("ls-tree", "-r", ref).splitlines() if line.startswith("160000 ")}


pins = gitlinks(base)
assert pins == gitlinks("HEAD"), "A product gitlink changed"
assert not git("diff", "--name-only", base, "--", "thirdparty"), "Dirty tracked submodule content"
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
upstream_native = git("ls-tree", "-r", "--name-only", tip, "--", cpp).splitlines()
assert all((REPO / path).is_file() for path in upstream_native), "Missing upstream Native destination"

print(json.dumps({
    "status": "PASS", "head": git("rev-parse", "HEAD"),
    "source_commits": len(recorded), "dispositions": dict(collections.Counter(c["status"] for c in ledger["commits"])),
    "protected_product_paths": "unchanged", "top_level_gitlinks": len(pins),
    "recursive_pins": "matched", "mechanical_native_moves": len(mapping),
    "mechanical_checkpoint": relocation_tip, "post_relocation_native_fixes": [fix["commit"] for fix in fixes],
    "upstream_native_destinations": "all present",
    "limit": "Source boundaries only; use separate build and device evidence for behavior."
}, indent=2))
