"""Regression checks for UnityFS integrity and Addressables dependency metadata."""
import base64
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from unity_addressables_compat import catalog_options, patch_catalog, unityfs_crc
from prepare_unity_texture_compat import atlas_page_size, validate_texture_references


def options_record(options):
    assembly = b'Unity.ResourceManager'
    kind = b'UnityEngine.ResourceManagement.ResourceProviders.AssetBundleRequestOptions'
    payload = json.dumps(options, separators=(',', ':')).encode('utf-16-le')
    return b'\x07' + bytes([len(assembly)]) + assembly + bytes([len(kind)]) + kind + struct.pack('<i', len(payload)) + payload


def catalog_fixture():
    options = {'m_Crc': 732020880, 'm_BundleSize': 123, 'm_Hash': 'a' * 32,
               'm_BundleName': 'test', 'm_UseUWRForLocalBundles': False, 'm_UseCrcForCachedBundles': True}
    first = options_record(options)
    second = options_record(dict(options, m_BundleName='untouched'))
    entries = struct.pack('<i', 2) + struct.pack('<7i', 0, 0, -1, 42, 0, 0, 0) + struct.pack('<7i', 1, 0, -1, 77, len(first), 1, 0)
    prefix = '{UnityEngine.AddressableAssets.Addressables.RuntimePath}/StandaloneWindows64/'
    return {'m_EntryDataString': base64.b64encode(entries).decode(),
            'm_ExtraDataString': base64.b64encode(first + second).decode(),
            'm_InternalIds': [prefix + 'map.bundle', prefix + 'other.bundle'],
            'm_InternalIdPrefixes': [], 'm_KeyDataString': 'unchanged', 'm_BucketDataString': 'unchanged'}


class AddressablesIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.catalog = catalog_fixture()
        self.override = {'sourceCrc': 732020880, 'sourceSize': 123, 'outputCrc': 2223761251,
                         'outputSize': 56789, 'outputSha256': 'b' * 64}

    def test_catalog_redirects_only_the_named_bundle(self):
        before = copy.deepcopy(self.catalog)
        updated, changes = patch_catalog(self.catalog, {'map.bundle': self.override})
        self.assertEqual(self.catalog, before)
        old_entries, old_extra, old_records = catalog_options(before)
        entries, extra, records = catalog_options(updated)
        self.assertEqual(entries[24:], old_entries[24:])
        self.assertEqual(entries[:20], old_entries[:20])
        self.assertEqual(extra[:len(old_extra)], old_extra)
        self.assertEqual(records[1]['options'], old_records[1]['options'])
        self.assertEqual(records[0]['options']['m_Crc'], 2223761251)
        self.assertEqual(records[0]['options']['m_BundleSize'], 56789)
        self.assertEqual(records[0]['options']['m_Hash'], 'b' * 32)
        self.assertTrue(records[0]['options']['m_UseCrcForCachedBundles'])
        self.assertEqual(updated['m_KeyDataString'], before['m_KeyDataString'])
        self.assertEqual(updated['m_BucketDataString'], before['m_BucketDataString'])
        self.assertEqual(len(changes), 1)

    def test_wrong_original_integrity_is_rejected(self):
        for field in ('sourceCrc', 'sourceSize'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                patch_catalog(self.catalog, {'map.bundle': dict(self.override, **{field: 1})})

    def test_crc_check_cannot_be_disabled(self):
        with self.assertRaises(ValueError):
            patch_catalog(self.catalog, {'map.bundle': dict(self.override, outputCrc=0)})

    def test_missing_or_duplicate_bundle_is_rejected(self):
        with self.assertRaises(ValueError):
            patch_catalog(self.catalog, {'missing.bundle': self.override})
        self.catalog['m_InternalIds'][1] = self.catalog['m_InternalIds'][0]
        with self.assertRaises(ValueError):
            patch_catalog(self.catalog, {'map.bundle': self.override})

    def test_remote_bundle_is_rejected(self):
        self.catalog['m_InternalIds'][0] = 'https://example.invalid/map.bundle'
        with self.assertRaises(ValueError):
            patch_catalog(self.catalog, {'map.bundle': self.override})

    def test_empty_overrides_preserve_catalog(self):
        self.assertEqual(patch_catalog(self.catalog, {})[0], self.catalog)


class UnityFSIntegrityTests(unittest.TestCase):
    def test_crc_uses_uncompressed_data_across_blocks_and_layouts(self):
        import lz4.block
        data = b'A' * 131072 + bytes(range(256)) * 129
        for compressed in (False, True):
            for at_end in (False, True):
                for padded in (False, True):
                    with self.subTest(compressed=compressed, at_end=at_end, padded=padded), tempfile.TemporaryDirectory() as temporary:
                        chunks = [data[:131072], data[131072:]]
                        packed = [lz4.block.compress(c, store_size=False) if compressed else c for c in chunks]
                        mode = 2 if compressed else 0
                        info = b'\0' * 16 + struct.pack('>I', len(chunks))
                        info += b''.join(struct.pack('>IIH', len(c), len(p), mode) for c, p in zip(chunks, packed))
                        info += struct.pack('>IQQI', 1, 0, len(data), 0) + b'data\0'
                        packed_info = lz4.block.compress(info, store_size=False) if compressed else info
                        flags = 64 | mode | (128 if at_end else 0) | (512 if padded else 0)
                        prefix = b'UnityFS\0' + struct.pack('>I', 8) + b'5.x.x\0' + b'2022.3.62f1\0'
                        header = prefix + struct.pack('>QIII', 0, len(packed_info), len(info), flags)
                        header += b'\0' * (-len(header) % 16)
                        leading_info = b'' if at_end else packed_info
                        padding = b'\0' * (-(len(header) + len(leading_info)) % 16) if padded else b''
                        body = leading_info + padding + b''.join(packed) + (packed_info if at_end else b'')
                        bundle = bytearray(header + body)
                        struct.pack_into('>Q', bundle, len(prefix), len(bundle))
                        path = Path(temporary) / 'fixture.bundle'
                        path.write_bytes(bundle)
                        self.assertEqual(unityfs_crc(path), zlib.crc32(data))

    def test_truncated_header_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'bad.bundle'
            path.write_bytes(b'UnityFS\0\0')
            with self.assertRaises(ValueError):
                unityfs_crc(path)


class SpineReferenceTests(unittest.TestCase):
    def setUp(self):
        def obj(pid, kind, text=None):
            return SimpleNamespace(path_id=pid, type=SimpleNamespace(name=kind),
                                   read=lambda: SimpleNamespace(m_Script=text))
        self.objects = {10: obj(10, 'Texture2D'), 11: obj(11, 'Material'),
                        12: obj(12, 'TextAsset', 'dragon.png\nsize:8108,7112\nfilter:Linear,Linear\n'),
                        13: obj(13, 'MonoBehaviour')}
        self.trees = {10: {}, 12: {},
                      11: {'m_SavedProperties': {'m_TexEnvs': [['_MainTex', {
                          'm_Texture': {'m_FileID': 0, 'm_PathID': 10},
                          'm_Scale': {'x': 1.0, 'y': 1.0}, 'm_Offset': {'x': 0.0, 'y': 0.0}}]]}},
                      13: {'atlasFile': {'m_FileID': 0, 'm_PathID': 12},
                           'materials': [{'m_FileID': 0, 'm_PathID': 11}]}}
        self.expected = {'name': 'dragon', 'referenceKind': 'spineMaterial', 'materialReferences': [11],
                         'atlasPages': [{'pathId': 12, 'page': 'dragon.png',
                                         'logicalSize': [8108, 7112], 'atlasAssetReferences': [13]}]}

    def validate(self):
        validate_texture_references(self.objects[10], self.objects, self.trees, self.expected, 1)

    def test_preserves_logical_atlas_size_independent_of_gpu_texture_size(self):
        self.validate()
        self.assertEqual(atlas_page_size('dragon.png\r\nsize: 8108, 7112\r\n', 'dragon.png'), [8108, 7112])

    def test_missing_zero_or_ambiguous_logical_size_is_rejected(self):
        for text in ('dragon.png\nfilter:Linear,Linear\n', 'dragon.png\nsize:0,7112\n',
                     'dragon.png\nsize:8108,7112\n\ndragon.png\nsize:8108,7112\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                atlas_page_size(text, 'dragon.png')

    def test_sprite_reference_is_rejected(self):
        self.objects[14] = SimpleNamespace(path_id=14, type=SimpleNamespace(name='Sprite'))
        self.trees[14] = {'texture': {'m_FileID': 0, 'm_PathID': 10}}
        with self.assertRaises(ValueError):
            self.validate()

    def test_non_identity_material_uv_and_missing_atlas_link_are_rejected(self):
        self.trees[11]['m_SavedProperties']['m_TexEnvs'][0][1]['m_Scale']['x'] = 0.5
        with self.assertRaises(ValueError):
            self.validate()
        self.setUp()
        self.trees[13]['materials'] = []
        with self.assertRaises(ValueError):
            self.validate()


if __name__ == '__main__':
    unittest.main()
