"""Smoke import libraries follow the compiler ISA, not the logical x64 slot."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("smoke_build_arch", ROOT / "automation/smoke.py")
smoke = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = smoke
spec.loader.exec_module(smoke)


class ImportArchitectureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = {"BUILD_DIR": str(self.root), "LLVM_MINGW": str(self.root / "tools")}
        self.wine = self.root / "wine-ohos-aarch64/dlls/vulkan-1"
        self.arch = patch.multiple(smoke, WINE_ARCH="aarch64", WINE_BUILD_DIRNAME="wine-ohos-aarch64")
        self.arch.start()
        self.addCleanup(self.arch.stop)

    def write(self, name, data=b"fixture"):
        path = self.wine / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def case(self, compiler=""):
        return smoke.Case(id="probe", exe="probe.exe", arch=["x64"],
                          build={"compiler": compiler} if compiler else {})

    def test_native_x86_and_explicit_amd64_use_their_own_import_architecture(self):
        for compiler, triple, slot, directory in (
                ("", "aarch64-w64-windows-gnu", "x64", "aarch64-windows"),
                ("", "i686-w64-mingw32", "x86", "i386-windows"),
                ("llvm-x86_64", "x86_64-w64-windows-gnu", "x64", "x86_64-windows")):
            expected = self.write(directory + "/libvulkan-1.a")
            with self.subTest(target=triple), patch.object(smoke.subprocess, "check_output", return_value=triple):
                self.assertEqual(smoke.resolve_vulkan_import(self.env, self.case(compiler), slot), expected)

    def test_arm64_wine_build_generates_amd64_imports_from_its_export_table(self):
        self.write("aarch64-windows/libvulkan-1.a", b"wrong ISA")
        self.write("aarch64-windows/vulkan-1.dll")
        exports = "\n".join("  Name: " + name for name in (
            "vkCreateInstance", "vkDestroyInstance", "vkGetInstanceProcAddr", "vkEnumeratePhysicalDevices"))
        with patch.object(smoke.subprocess, "check_output", side_effect=["x86_64-w64-windows-gnu", exports]), \
                patch.object(smoke.subprocess, "run") as run:
            library = smoke.resolve_vulkan_import(self.env, self.case("llvm-x86_64"), "x64")
        self.assertEqual(library, self.root / "smoke-imports/x86_64/libvulkan-1.a")
        command = run.call_args.args[0]
        self.assertEqual(command[command.index("-m") + 1], "i386:x86-64")
        self.assertIn("vkEnumeratePhysicalDevices", library.with_name("vulkan-1.def").read_text())

    def test_missing_native_import_is_not_silently_replaced(self):
        with patch.object(smoke.subprocess, "check_output", return_value="i686-w64-mingw32"), \
                self.assertRaises(SystemExit):
            smoke.resolve_vulkan_import(self.env, self.case(), "x86")

    def test_invalid_export_source_is_rejected(self):
        self.write("aarch64-windows/vulkan-1.dll")
        with patch.object(smoke.subprocess, "check_output", side_effect=["x86_64-w64-windows-gnu", "Name: unrelated"]), \
                patch.object(smoke.subprocess, "run") as run, self.assertRaises(SystemExit):
            smoke.resolve_vulkan_import(self.env, self.case("llvm-x86_64"), "x64")
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
