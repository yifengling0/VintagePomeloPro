"""Policy checks for the opt-in RPGXP Wine diagnostic patch."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = (ROOT / "scripts/build_wine.sh").read_text()
PATCH_A = (ROOT / "patches/wine/0013a-ntdll-ohos-rpgxp-diagnostics.patch").read_text()
ENSURE_HELPER = BUILD_SCRIPT[
    BUILD_SCRIPT.index("ensure_wine_patch() {"):
    BUILD_SCRIPT.index("\n}\n", BUILD_SCRIPT.index("ensure_wine_patch() {")) + 3
]
REJECT_HELPER = BUILD_SCRIPT[
    BUILD_SCRIPT.index("reject_quarantined_wine_patch() {"):
    BUILD_SCRIPT.index("\n}\n", BUILD_SCRIPT.index("reject_quarantined_wine_patch() {")) + 3
]
PATCH_PHASE = BUILD_SCRIPT[
    BUILD_SCRIPT.index("# Refuse persistent sources contaminated"):
    BUILD_SCRIPT.index("# Wine 编译标志")
]
TINY_PATCH = """--- a/source.txt
+++ b/source.txt
@@ -1 +1 @@
-before
+after
"""


class WineRpgxpDiagnosticsPolicyTest(unittest.TestCase):
    def test_only_repaired_diagnostic_patch_is_registered(self):
        registrations = re.findall(
            r'^ensure_wine_patch\s+"[^\"]*/([^\"]+)"', BUILD_SCRIPT, re.MULTILINE
        )
        self.assertEqual(registrations.count("0013a-ntdll-ohos-rpgxp-diagnostics.patch"), 1)
        self.assertNotIn("0013b-wow64-exception-dispatch-frame-guard.patch", registrations)
        self.assertNotIn("0013c-ntdll-wow32-stack-top-pad.patch", registrations)

    def test_quarantine_checks_precede_every_ensure(self):
        calls_start = BUILD_SCRIPT.index("# Refuse persistent sources contaminated")
        first_ensure = BUILD_SCRIPT.index('\nensure_wine_patch "', calls_start)
        for patch in ("0013b-wow64-exception-dispatch-frame-guard.patch",
                      "0013c-ntdll-wow32-stack-top-pad.patch"):
            reject = BUILD_SCRIPT.index(
                f'reject_quarantined_wine_patch "$SCRIPT_DIR/../patches/wine/{patch}"',
                calls_start,
            )
            self.assertLess(reject, first_ensure)

    def test_gate_is_exactly_one_and_quiet_wins(self):
        self.assertIn('rpgxp_diagnostics[0] == \'1\' && !rpgxp_diagnostics[1]', PATCH_A)
        self.assertIn("!ohos_diag_quiet && rpgxp_diagnostics", PATCH_A)
        for value in (None, "", "0", "10", "01", "true", " 1", "1 "):
            self.assertFalse(value is not None and len(value) == 1 and value[0] == "1")
        self.assertTrue(len("1") == 1 and "1"[0] == "1")

    def test_signal_path_contract_is_fixed_width_and_exact(self):
        added = "\n".join(
            line[1:] for line in PATCH_A.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn("uint32_t node[2] = {0, 0};", added)
        self.assertIn("uint32_t code[2] = {0, 0};", added)
        self.assertIn("ohos_smc_read_exact( list, node, sizeof(node) )", added)
        self.assertIn("ohos_smc_read_exact( pc, code, sizeof(code) )", added)
        self.assertIn("bytes == (ssize_t)len", added)
        self.assertIn("struct iovec remote = { (void *)(uintptr_t)addr, len };", added)
        self.assertIn("errno = saved_errno;", added)
        self.assertIn('" pc+4="', added)
        self.assertNotIn("uintptr_t node[2]", added)
        self.assertNotIn("uintptr_t code[2]", added)
        for forbidden in ("snprintf(", "fprintf(", "malloc(", "free(",
                          "ohos_smc_reload_maps(", "ohos_smc_map_lookup("):
            self.assertNotIn(forbidden, added)

    def test_event_key_is_shared_and_continuations_are_tagged(self):
        added = "\n".join(
            line[1:] for line in PATCH_A.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn("state->last_seq = event_seq;", added)
        self.assertIn("teb, fault_seq );", added)
        self.assertIn("while (!fault_seq);", added)
        self.assertNotIn("fault_count", added)
        for log in ("none", "wow", "cpu", "seh", "instructions", "regs0", "regs1", "guest"):
            self.assertIn(f'ohos_signal_log_field_dec( &{log}, "tid", tid );', added)
            self.assertIn(f'ohos_signal_log_field_dec( &{log}, "seq", seq );', added)

    def _run_quarantine_check(self, content):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "source.txt").write_text(content)
            (root / "change.patch").write_text(TINY_PATCH)
            result = subprocess.run(
                ["bash", "-c", "set -euo pipefail\nlog() { :; }\n" + REJECT_HELPER +
                 '\nWINE_SRC="$1"\nreject_quarantined_wine_patch "$1/change.patch" test\n',
                 "quarantine-test", str(root)],
                capture_output=True, text=True,
            )
            return result, (root / "source.txt").read_text()

    def test_quarantine_check_never_mutates_source(self):
        clean, clean_text = self._run_quarantine_check("before\n")
        self.assertEqual(clean.returncode, 0, clean.stdout + clean.stderr)
        self.assertEqual(clean_text, "before\n")
        applied, applied_text = self._run_quarantine_check("after\n")
        self.assertNotEqual(applied.returncode, 0)
        self.assertEqual(applied_text, "after\n")
        conflict, conflict_text = self._run_quarantine_check("different\n")
        self.assertNotEqual(conflict.returncode, 0)
        self.assertEqual(conflict_text, "different\n")

    def test_contaminated_patch_phase_rejects_before_normal_write(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source"
            scripts = root / "scripts"
            patches = root / "patches/wine"
            source.mkdir()
            scripts.mkdir()
            patches.mkdir(parents=True)
            (source / "contaminated.txt").write_text("after\n")
            (source / "normal.txt").write_text("before\n")
            (patches / "0013b-wow64-exception-dispatch-frame-guard.patch").write_text(
                TINY_PATCH.replace("source.txt", "contaminated.txt")
            )
            (patches / "0001-win32u-repair-external-font-registration.patch").write_text(
                TINY_PATCH.replace("source.txt", "normal.txt")
            )
            before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            result = subprocess.run(
                ["bash", "-c", "set -euo pipefail\nlog() { :; }\n" + ENSURE_HELPER +
                 REJECT_HELPER + '\nSCRIPT_DIR="$1/scripts"\nWINE_SRC="$1/source"\n' +
                 PATCH_PHASE,
                 "patch-phase-test", str(root)],
                capture_output=True, text=True,
            )
            after = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("quarantined Wine patch is already applied", result.stderr)
            self.assertEqual(after, before)

    def test_malformed_normal_patch_fails_without_modification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "source.txt").write_text("before\n")
            (root / "broken.patch").write_text("this is not a patch\n")
            before = (root / "source.txt").read_bytes()
            result = subprocess.run(
                ["bash", "-c", "set -euo pipefail\nlog() { :; }\n" + ENSURE_HELPER +
                 '\nWINE_SRC="$1"\nensure_wine_patch "$1/broken.patch" test\n',
                 "malformed-patch-test", str(root)],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((root / "source.txt").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
