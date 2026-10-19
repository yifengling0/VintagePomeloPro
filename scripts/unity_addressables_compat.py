"""Integrity metadata for reviewed local UnityFS/Addressables texture overrides.

CRC is computed over decompressed UnityFS blocks (not the compressed file).
Catalog updates append a request-options record and redirect only its bundle
entry, preserving every other extra-data offset and dependency reference.
"""
import base64
import copy
import json
from pathlib import Path
import struct
import zlib


def unityfs_crc(path):
    import lz4.block

    path = Path(path)
    with path.open('rb') as stream:
        def read(count):
            result = stream.read(count)
            if len(result) != count:
                raise ValueError('Truncated UnityFS bundle')
            return result

        def cstring():
            data = bytearray()
            while (value := read(1)) != b'\0':
                data.extend(value)
                if len(data) > 1024:
                    raise ValueError('Invalid UnityFS header string')
            return bytes(data)

        if cstring() != b'UnityFS':
            raise ValueError('A reviewed UnityFS bundle is required')
        version, = struct.unpack('>I', read(4))
        cstring()
        engine = cstring().decode('ascii')
        # Older CN bundles reuse flag bits for encryption; do not guess.
        if version < 7 or not engine.startswith('2022.'):
            raise ValueError('Only reviewed Unity 2022 UnityFS v7+ layout is supported')
        size, compressed, uncompressed, flags = struct.unpack('>QIII', read(20))
        if size != path.stat().st_size:
            raise ValueError('UnityFS file size mismatch')
        if flags & ~(0x3f | 0x40 | 0x80 | 0x200):
            raise ValueError('Unsupported UnityFS layout flags')
        stream.seek((-stream.tell()) % 16, 1)
        start = stream.tell()

        def unpack(data, length, mode):
            if mode == 0:
                result = data
            elif mode in (2, 3):
                result = lz4.block.decompress(data, uncompressed_size=length)
            else:
                raise ValueError(f'Unsupported UnityFS compression: {mode}')
            if len(result) != length:
                raise ValueError('UnityFS decompressed block size mismatch')
            return result

        if flags & 128:
            stream.seek(size - compressed)
        info = unpack(read(compressed), uncompressed, flags & 63)
        count, = struct.unpack_from('>I', info, 16)
        if 20 + count * 10 > len(info):
            raise ValueError('Invalid UnityFS block table')
        blocks = [struct.unpack_from('>IIH', info, 20 + i * 10) for i in range(count)]
        stream.seek(start if flags & 128 else start + compressed)
        if flags & 512:
            stream.seek((-stream.tell()) % 16, 1)
        result = 0
        for length, packed, mode in blocks:
            if mode & ~63:
                raise ValueError('Unsupported UnityFS block flags')
            result = zlib.crc32(unpack(read(packed), length, mode & 63), result)
        return result


def catalog_options(catalog):
    entries = base64.b64decode(catalog['m_EntryDataString'], validate=True)
    extra = base64.b64decode(catalog['m_ExtraDataString'], validate=True)
    count, = struct.unpack_from('<i', entries)
    if count < 0 or len(entries) != 4 + count * 28 or catalog.get('m_InternalIdPrefixes'):
        raise ValueError('Unsupported Addressables catalog layout')
    records = []
    for index in range(count):
        entry = struct.unpack_from('<7i', entries, 4 + index * 28)
        if entry[4] < 0:
            continue
        offset = entry[4]
        if extra[offset] != 7:
            continue  # Non-JSON extra-data records are preserved verbatim.
        cursor = offset + 1
        cursor += 1 + extra[cursor]  # assembly name
        length = extra[cursor]
        cursor += 1
        class_name = extra[cursor:cursor + length].decode('ascii')
        cursor += length
        if class_name != 'UnityEngine.ResourceManagement.ResourceProviders.AssetBundleRequestOptions':
            continue
        length, = struct.unpack_from('<i', extra, cursor)
        if length < 0 or cursor + 4 + length > len(extra):
            raise ValueError('Invalid Addressables options length')
        options = json.loads(extra[cursor + 4:cursor + 4 + length].decode('utf-16-le'))
        records.append({'index': index, 'entry': entry, 'offset': offset,
                        'headerEnd': cursor, 'options': options,
                        'internalId': catalog['m_InternalIds'][entry[0]]})
    return entries, extra, records


def patch_catalog(catalog, overrides):
    """Return a catalog copy; validate the old CRC and preserve CRC checking."""
    result = copy.deepcopy(catalog)
    entries, extra, records = catalog_options(catalog)
    new_entries, new_extra = bytearray(entries), bytearray(extra)
    updates = []
    for name, override in overrides.items():
        matches = [r for r in records if r['internalId'].replace('\\', '/').split('/')[-1] == name]
        if len(matches) != 1:
            raise ValueError(f'Expected one local bundle catalog record: {name}')
        record = matches[0]
        before = record['options']
        if (not record['internalId'].startswith('{UnityEngine.AddressableAssets.Addressables.RuntimePath}')
                or before.get('m_UseUWRForLocalBundles') is not False):
            raise ValueError('Only direct local AssetBundleProvider records are supported')
        if (not override['sourceCrc'] or before.get('m_Crc') != override['sourceCrc']
                or before.get('m_BundleSize') != override['sourceSize']):
            raise ValueError(f'Original catalog CRC or size mismatch: {name}')
        if not override['outputCrc']:
            raise ValueError('An override must retain a nonzero CRC')
        after = dict(before, m_Crc=override['outputCrc'], m_BundleSize=override['outputSize'],
                     m_Hash=override['outputSha256'][:32])
        payload = json.dumps(after, separators=(',', ':')).encode('utf-16-le')
        encoded = extra[record['offset']:record['headerEnd']] + struct.pack('<i', len(payload)) + payload
        struct.pack_into('<i', new_entries, 4 + record['index'] * 28 + 16, len(new_extra))
        new_extra.extend(encoded)
        updates.append({'name': name, 'entry': record['index'], 'before': before, 'after': after})
    result['m_EntryDataString'] = base64.b64encode(new_entries).decode('ascii')
    result['m_ExtraDataString'] = base64.b64encode(new_extra).decode('ascii')
    # Re-read using the same layout as ContentCatalogData.CreateLocator.
    _, _, verified = catalog_options(result)
    indexed = {r['index']: r for r in verified}
    expected = {u['entry']: u['after'] for u in updates}
    for record in records:
        actual = indexed[record['index']]
        if actual['options'] != expected.get(record['index'], record['options']):
            raise ValueError('Catalog request-options verification failed')
        if actual['internalId'] != record['internalId']:
            raise ValueError('Catalog resource reference changed')
    return result, updates
