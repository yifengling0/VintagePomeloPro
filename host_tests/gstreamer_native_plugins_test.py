"""Exercise the package contract with real ELF fixtures and both runtime routes."""
from pathlib import Path
import hashlib
import io
import json
import struct
import subprocess
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from gstreamer_inventory import PLUGINS, describe, fingerprint, write_header
from check_runtime_components import Archive, ValidationError, validate_gstreamer, validate_media_dependency_closure


def archive(files):
    stream = io.BytesIO()
    with ZipFile(stream, "w") as z:
        for path, data in files.items():
            z.writestr(path, data)
    stream.seek(0)
    return Archive(stream)


def rejected(payload, hap, fragment):
    try:
        validate_gstreamer(payload, "aarch64", hap)
    except ValidationError as error:
        assert fragment in str(error), str(error)
    else:
        raise AssertionError("accepted invalid native plugin package")


with tempfile.TemporaryDirectory(prefix="gst-inventory-") as temporary:
    base = Path(temporary)
    # A real host shared object exercises the ELF parser, then a header machine
    # field change models a wrong-ABI plugin without depending on an OHOS SDK.
    source = base / "unit.c"
    source.write_text("int gst_plugin_fixture(void) { return 1; }\n")
    subprocess.run(["cc", "-shared", "-nostdlib", "-fPIC", str(source), "-o", str(base / "fixture.so")], check=True)
    x64 = (base / "fixture.so").read_bytes()
    arm64 = bytearray(x64); struct.pack_into("<H", arm64, 18, 183); arm64 = bytes(arm64)
    plugins = base / "plugins"; plugins.mkdir()
    for name in PLUGINS:
        (plugins / name).write_bytes(arm64)
    gst = describe(plugins, "aarch64", "arm64-v8a")
    write_header(base / "inventory.h", gst)
    assert gst["inventorySha256"] in (base / "inventory.h").read_text()
    manifest = {"schemaVersion": 2, "wineArch": "aarch64", "gstreamer": gst}
    payload_files = {"runtime-components.json": json.dumps(manifest)}
    native_files = {"libs/arm64-v8a/" + name: arm64 for name in PLUGINS}
    native_files["libs/arm64-v8a/winegstreamer.so"] = arm64 + ("winehua-gst-inventory:" + gst["inventorySha256"]).encode()
    with_payload = archive(payload_files)
    native = archive(native_files)
    validate_gstreamer(with_payload, "aarch64", native)
    validate_media_dependency_closure(with_payload, native, "aarch64")
    validate_gstreamer(with_payload, "aarch64")  # Payload-only check is explicitly partial.
    bad = dict(native_files); bad["libs/arm64-v8a/winegstreamer.so"] = arm64
    rejected(with_payload, archive(bad), "inventory fingerprint")
    # Dependencies copied only to rawfile are not executable in the native namespace.
    dep = base / "dep.c"; dep.write_text("int gst_test_dep = 1;\n")
    subprocess.run(["cc", "-shared", "-nostdlib", "-Wl,-soname,libgst_test_dep.so", str(dep), "-o", str(base / "libgst_test_dep.so")], check=True)
    subprocess.run(["cc", "-shared", "-nostdlib", "-Wl,--no-as-needed", str(source), "-L" + str(base), "-lgst_test_dep", "-o", str(base / "with-needed.so")], check=True)
    dependent = bytearray((base / "with-needed.so").read_bytes()); struct.pack_into("<H", dependent, 18, 183)
    dependent = bytes(dependent)
    dependency_manifest = json.loads(json.dumps(manifest))
    dependency_manifest["gstreamer"]["files"][PLUGINS[0]] = hashlib.sha256(dependent).hexdigest()
    dependency_manifest["gstreamer"]["inventorySha256"] = fingerprint(dependency_manifest["gstreamer"]["files"])
    raw_dep = {"runtime-components.json": json.dumps(dependency_manifest), "bin/aarch64-unix/libgst_test_dep.so": arm64}
    bad = dict(native_files); bad["libs/arm64-v8a/" + PLUGINS[0]] = dependent
    rejected(archive(raw_dep), archive(bad), "missing required file: libs/arm64-v8a/libgst_test_dep.so")
    bad = dict(native_files); del bad["libs/arm64-v8a/libgstlibav.so"]
    rejected(with_payload, archive(bad), "packaged GStreamer plugin inventory")
    stale = dict(payload_files); stale["bin/aarch64-unix/gstreamer-1.0/libgstlibav.so"] = arm64
    rejected(archive(stale), native, "el2 payload")
    bad = dict(native_files); bad["libs/arm64-v8a/libgstlibav.so"] = x64
    rejected(with_payload, archive(bad), "plugin checksum")
    # A self-consistent manifest cannot hide a foreign-ABI plugin.
    gst["files"]["libgstlibav.so"] = hashlib.sha256(x64).hexdigest()
    gst["inventorySha256"] = fingerprint(gst["files"])
    payload_files["runtime-components.json"] = json.dumps(manifest)
    rejected(archive(payload_files), archive(bad), "expected aarch64 ELF64")
    for name in PLUGINS:
        (plugins / name).write_bytes(x64)
    box64 = describe(plugins, "x86_64", "arm64-v8a")
    direct = describe(plugins, "x86_64", "x86_64")
    assert box64["storage"] == "payload" and direct["storage"] == "native-libs"
    payload = {"runtime-components.json": json.dumps({"schemaVersion": 2, "wineArch": "x86_64", "gstreamer": box64})}
    payload.update({"bin/x86_64-unix/gstreamer-1.0/" + name: x64 for name in PLUGINS})
    validate_gstreamer(archive(payload), "x86_64")
    old = box64["inventorySha256"]
    (plugins / PLUGINS[0]).write_bytes(x64 + b"new plugin build")
    assert describe(plugins, "x86_64", "arm64-v8a")["inventorySha256"] != old
print("gstreamer_native_plugins_test: PASS")
