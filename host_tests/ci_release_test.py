#!/usr/bin/env python3
"""Offline source provenance and unsigned/signed packaging regression tests."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ci_release", ROOT / "scripts/verify_ci_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
APP = {"bundleName": "com.vintage.pomelopro", "versionName": "1.3.3", "versionCode": 1003003}


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True,
                                   stderr=subprocess.PIPE).strip()


def init_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    git(root, "init", "-q")
    git(root, "config", "user.name", "CI test")
    git(root, "config", "user.email", "ci-test@example.invalid")
    write(root / "AppScope/app.json5", json.dumps({"app": APP}))
    commit(root)


def commit(root):
    git(root, "add", ".")
    git(root, "commit", "-qm", "test fixture")


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="winehua-ci-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "root"
        init_repo(self.root)
        self.sha = git(self.root, "rev-parse", "HEAD")

    def test_source_and_annotated_tag(self):
        git(self.root, "tag", "-am", "RC", "rc-1.3.3")
        result = release.verify_source(self.root, self.sha, "rc-1.3.3")
        self.assertEqual(result["app"], APP)
        self.assertEqual(result["commit"], self.sha)

    def test_fetch_annotated_tag_preserves_checkout_lightweight_ref(self):
        git(self.root, "tag", "-am", "RC", "rc-1.3.3")
        checkout = Path(self.temp.name) / "checkout"
        checkout.mkdir()
        git(checkout, "init", "-q")
        git(checkout, "fetch", "--depth=1", "--no-tags", str(self.root), self.sha)
        git(checkout, "checkout", "-q", "--detach", "FETCH_HEAD")
        # Matches actions/checkout's shallow, SHA-targeted tag representation.
        git(checkout, "tag", "rc-1.3.3", self.sha)
        git(checkout, "fetch", "--depth=1", "--no-tags", str(self.root), "refs/tags/rc-1.3.3")
        self.assertEqual(git(checkout, "rev-parse", "FETCH_HEAD^{commit}"), self.sha)
        self.assertEqual(git(checkout, "cat-file", "-t", "refs/tags/rc-1.3.3"), "commit")
        self.assertEqual(release.verify_source(checkout, self.sha, "rc-1.3.3")["commit"], self.sha)

    def test_sha_checkout_without_local_tag_is_recreated(self):
        git(self.root, "tag", "-am", "RC", "rc-1.3.3")
        checkout = Path(self.temp.name) / "checkout"
        checkout.mkdir()
        git(checkout, "init", "-q")
        git(checkout, "fetch", "--depth=1", "--no-tags", str(self.root), self.sha)
        git(checkout, "checkout", "-q", "--detach", "FETCH_HEAD")
        git(checkout, "fetch", "--depth=1", "--no-tags", str(self.root), "refs/tags/rc-1.3.3")
        with self.assertRaisesRegex(ValueError, "not in checkout"):
            release.verify_source(checkout, self.sha, "rc-1.3.3")
        git(checkout, "tag", "rc-1.3.3", self.sha)
        self.assertEqual(release.verify_source(checkout, self.sha, "rc-1.3.3")["commit"], self.sha)

    def test_wrong_sha_and_version(self):
        with self.assertRaises(ValueError):
            release.verify_source(self.root, "0" * 40, "")
        git(self.root, "tag", "rc-1.3.2")
        with self.assertRaisesRegex(ValueError, "versionName"):
            release.verify_source(self.root, self.sha, "rc-1.3.2")

    def test_tag_must_point_to_built_commit(self):
        git(self.root, "tag", "rc-1.3.3")
        write(self.root / "another.txt", "changed")
        commit(self.root)
        with self.assertRaisesRegex(ValueError, "different commit"):
            release.verify_source(self.root, git(self.root, "rev-parse", "HEAD"), "rc-1.3.3")

    def test_recursive_gitlinks_not_branch_tips(self):
        child = self.root / "thirdparty/test"
        init_repo(child)
        git(self.root, "submodule", "add", "./thirdparty/test", "thirdparty/test")
        commit(self.root)
        sha = git(self.root, "rev-parse", "HEAD")
        result = release.verify_source(self.root, sha, "")
        self.assertEqual(result["submodules"]["thirdparty/test"], git(child, "rev-parse", "HEAD"))
        write(child / "other.txt", "newer branch tip")
        commit(child)
        with self.assertRaisesRegex(ValueError, "gitlink mismatch"):
            release.verify_source(self.root, sha, "")
        child.rename(self.root / "saved-child")
        child.mkdir()
        with self.assertRaisesRegex(ValueError, "not initialized"):
            release.verify_source(self.root, sha, "")

    def test_dirty_source_fails(self):
        write(self.root / "AppScope/app.json5", json.dumps({"app": dict(APP, vendor="dirty")}))
        with self.assertRaises(subprocess.CalledProcessError):
            release.verify_source(self.root, self.sha, "")

    def test_artifact_version_and_architecture(self):
        manifest = {"commit": self.sha, "releaseTag": "rc-1.3.3", "app": APP}
        hap = self.root / "test.hap"
        def create(app, machine=183):
            with zipfile.ZipFile(hap, "w") as archive:
                archive.writestr("module.json", json.dumps({"app": dict(app, minAPIVersion=60100023, targetAPIVersion=60100023)}))
                header = b"\x7fELF\x02\x01" + bytes(12) + machine.to_bytes(2, "little")
                for name in ("box64.so", "libentry.so", "libwine_child.so", "libwinehua_vtest_server.so",
                             "libvirglrenderer.so.1", "libvirgl_child.so"):
                    archive.writestr("libs/arm64-v8a/" + name, header)
                archive.writestr("resources/rawfile/wine-data.zip", b"fixture")
        create(APP)
        self.assertEqual(release.verify_artifact(hap, manifest)["app"], APP)
        create(dict(APP, versionCode=1003002))
        with self.assertRaisesRegex(ValueError, "versionCode"):
            release.verify_artifact(hap, manifest)
        create(APP, 62)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            release.verify_artifact(hap, manifest)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="winehua-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("package.sh", "env.sh"):
            write(self.root / "scripts" / name, (ROOT / "scripts" / name).read_text(encoding="utf-8"))
        write(self.root / "AppScope/app.json5", json.dumps({"app": APP}))
        write(self.root / "entry/build-profile.json5", '{"abiFilters": ["arm64-v8a"]}')
        write(self.root / "entry/src/main/module.json5", '{}')
        self.profile = {"app": {"signingConfigs": [], "products": [{"name": "default",
                        "targetSdkVersion": "6.1.0(23)", "compatibleSdkVersion": "6.1.0(23)"}]}}
        write(self.root / "build-profile.json5", json.dumps(self.profile))
        write(self.root / "sign.py", 'raise SystemExit("unexpected signing invocation")')
        self.hvigor = self.root / "tools/bin/hvigorw"
        write(self.hvigor, '#!/bin/bash\nmkdir -p entry/build/default/outputs/default\nprintf fixture > entry/build/default/outputs/default/entry-default-unsigned.hap\n')
        self.hvigor.chmod(0o755)
        self.env = dict(os.environ, TOOL_HOME=str(self.root / "tools"), WAYLAND_SCANNER="/bin/true",
                        NATIVE_ARCH="arm64-v8a", USER_PROFILE_DIR=str(self.root / "mount"),
                        USER_SIGNATURE_DIR=str(self.root / "signatures"))

    def run_package(self, target):
        return subprocess.run(["bash", str(self.root / "scripts/package.sh"), target],
                              env=self.env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_unsigned_does_not_sign_or_import_private_mount(self):
        write(self.root / "mount/build-profile.json5", "private invalid profile must not be imported")
        (self.root / "signatures").mkdir()
        result = self.run_package("hap-unsigned")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.root / "entry/build/default/outputs/default/entry-default-signed.hap").exists())
        self.assertEqual(json.loads((self.root / "build-profile.json5").read_text()), self.profile)

    def test_signed_default_still_requires_signing(self):
        result = self.run_package("hap")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("signingConfigs is empty", result.stdout)
        self.assertFalse((self.root / "entry/build").exists())

    def test_unsigned_rejects_signing_profile(self):
        self.profile["app"]["signingConfigs"] = [{"name": "private"}]
        write(self.root / "build-profile.json5", json.dumps(self.profile))
        result = self.run_package("hap-unsigned")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsigned profile", result.stdout)

    def test_missing_unsigned_output_is_failure(self):
        write(self.hvigor, '#!/bin/bash\nexit 0\n')
        result = self.run_package("hap-unsigned")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsigned HAP missing", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
