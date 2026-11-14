import io
import struct
from zipfile import ZipFile
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_candidate_hap import (ELF_HEADER, PROGRAM_HEADER, load_signature,
                                 ValidationError, verify_ui_payload)


def make_elf(section_offset: int, section_count: int) -> bytearray:
    image = bytearray(512)
    ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    ELF_HEADER.pack_into(
        image, 0, ident, 3, 183, 1, 0, ELF_HEADER.size, section_offset, 0,
        ELF_HEADER.size, PROGRAM_HEADER.size, 1, 64, section_count, 1,
    )
    PROGRAM_HEADER.pack_into(
        image, ELF_HEADER.size, 1, 5, 0, 0, 0, len(image), len(image), 0x1000,
    )
    image[160:176] = b"candidate-payload"
    return image


class CandidateHapElfTest(unittest.TestCase):
    def test_release_section_table_changes_do_not_change_runtime_signature(self) -> None:
        built = make_elf(256, 3)
        packaged = make_elf(0, 0)
        self.assertEqual(load_signature("built", built), load_signature("packaged", packaged))

    def test_loadable_byte_change_fails_runtime_signature(self) -> None:
        built = make_elf(256, 3)
        packaged = make_elf(0, 0)
        packaged[160] ^= 0xFF
        self.assertNotEqual(load_signature("built", built), load_signature("packaged", packaged))


class CandidateHapUiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.compiled = {
            "ets/modules.abc": b"compiled ABC references icon 16777255",
            "resources.index": b"16777255=window.svg;16777282=gear.svg",
            "module.json": b'{"startWindowIconId":16777255}',
            "resources/base/media/window.svg": b"window image",
            "resources/base/media/gear.svg": b"gear image",
            "resources/rawfile/wine-data.zip": b"current runtime",
        }

    def check(self, members: dict[str, bytes]) -> None:
        def archive(data: dict[str, bytes]) -> io.BytesIO:
            output = io.BytesIO()
            with ZipFile(output, "w") as hap:
                for name, content in data.items():
                    hap.writestr(name, content)
            output.seek(0)
            return output
        with ZipFile(archive(members)) as candidate, ZipFile(archive(self.compiled)) as compiled:
            verify_ui_payload(candidate, compiled)

    def test_atomic_ui_with_baseline_raw_runtime_passes(self) -> None:
        self.check({**self.compiled, "resources/rawfile/wine-data.zip": b"verified baseline runtime"})

    def test_new_abc_with_old_resource_index_fails(self) -> None:
        with self.assertRaisesRegex(ValidationError, "resources.index"):
            self.check({**self.compiled, "resources.index": b"16777282=window.svg;16777255=gear.svg"})

    def test_old_module_metadata_fails(self) -> None:
        with self.assertRaisesRegex(ValidationError, "module.json"):
            self.check({**self.compiled, "module.json": b'{"startWindowIconId":16777282}'})

    def test_changed_or_missing_ui_asset_fails(self) -> None:
        name = "resources/base/media/window.svg"
        with self.assertRaisesRegex(ValidationError, "window.svg"):
            self.check({**self.compiled, name: b"wrong image"})
        with self.assertRaisesRegex(ValidationError, "inventory mismatch"):
            self.check({key: value for key, value in self.compiled.items() if key != name})


if __name__ == "__main__":
    unittest.main()
