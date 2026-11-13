#!/usr/bin/env python3
"""Prepare reviewed Unity texture overrides without changing Direct3D feature levels.

This offline tool pins each original bundle and explicitly names the reviewed
textures. It supports full-UV RawImages and reviewed Spine main textures with
explicit logical atlas sizes. It never edits the source game or sprite geometry.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

from unity_addressables_compat import patch_catalog, unityfs_crc


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def image_size(width, height, limit):
    divisor = 1
    while max(width // divisor, height // divisor) > limit:
        divisor *= 2
    return max(1, width // divisor), max(1, height // divisor)


def includes_pointer(value, target):
    if isinstance(value, dict):
        if value.get('m_FileID') == 0 and value.get('m_PathID') == target:
            return True
        return any(includes_pointer(child, target) for child in value.values())
    return isinstance(value, (list, tuple)) and any(includes_pointer(child, target) for child in value)


def atlas_page_size(text, page):
    if isinstance(text, bytes):
        text = text.decode('utf8')
    pattern = (r'(?:^|\n)' + re.escape(page)
               + r'\r?\nsize:[ \t]*(\d+)[ \t]*,[ \t]*(\d+)[ \t]*(?=\r?\n|$)')
    matches = re.findall(pattern, text)
    if len(matches) != 1 or any(int(n) <= 0 for n in matches[0]):
        raise ValueError('Spine page requires one explicit nonzero logical size')
    return [int(n) for n in matches[0]]


def validate_texture_references(obj, objects, trees, expected, index_path_id):
    refs = []
    kind = expected.get('referenceKind', 'rawImage')
    if kind not in ('rawImage', 'spineMaterial'):
        raise ValueError('Unsupported texture reference kind')
    for other in objects.values():
        if other.path_id == obj.path_id:
            continue
        tree = trees[other.path_id]
        if not includes_pointer(tree, obj.path_id):
            continue
        if other.type.name == 'AssetBundle' and other.path_id == index_path_id:
            remainder = {k: v for k, v in tree.items() if k not in ('m_PreloadTable', 'm_Container')}
            if includes_pointer(remainder, obj.path_id):
                raise ValueError('Unreviewed AssetBundle texture reference')
            continue
        if kind == 'rawImage':
            if (other.type.name != 'MonoBehaviour'
                    or tree.get('m_Texture') != {'m_FileID': 0, 'm_PathID': obj.path_id}
                    or tree.get('m_UVRect') != {'x': 0.0, 'y': 0.0, 'width': 1.0, 'height': 1.0}):
                raise ValueError('Unsupported RawImage texture reference')
        else:
            if other.type.name != 'Material':
                raise ValueError('Unsupported Spine texture reference')
            slots = tree.get('m_SavedProperties', {}).get('m_TexEnvs', [])
            main = [v for k, v in slots if k == '_MainTex']
            if (len(main) != 1 or main[0] != {
                    'm_Texture': {'m_FileID': 0, 'm_PathID': obj.path_id},
                    'm_Scale': {'x': 1.0, 'y': 1.0}, 'm_Offset': {'x': 0.0, 'y': 0.0}}
                    or any(includes_pointer(v, obj.path_id) for k, v in slots if k != '_MainTex')):
                raise ValueError('Unreviewed Spine material texture slot')
            remainder = {k: v for k, v in tree.items() if k != 'm_SavedProperties'}
            saved = {k: v for k, v in tree.get('m_SavedProperties', {}).items() if k != 'm_TexEnvs'}
            if includes_pointer(remainder, obj.path_id) or includes_pointer(saved, obj.path_id):
                raise ValueError('Unreviewed Spine material texture reference')
        refs.append(other.path_id)
    approved = expected['rawImageReferences' if kind == 'rawImage' else 'materialReferences']
    if not approved or sorted(refs) != sorted(approved):
        raise ValueError('Unreviewed texture references')
    if kind == 'spineMaterial':
        pages = expected['atlasPages']
        if not pages:
            raise ValueError('Spine texture requires reviewed atlas pages')
        covered_materials = set()
        for page in pages:
            if page['page'] != expected['name'] + '.png':
                raise ValueError('Spine atlas page name must match the texture')
            text_obj = objects[page['pathId']]
            if text_obj.type.name != 'TextAsset':
                raise ValueError('Spine atlas must be a TextAsset')
            if atlas_page_size(text_obj.read().m_Script, page['page']) != page['logicalSize']:
                raise ValueError('Spine logical page size changed')
            for atlas_id in page['atlasAssetReferences']:
                atlas = trees[atlas_id]
                if (objects[atlas_id].type.name != 'MonoBehaviour'
                        or atlas.get('atlasFile') != {'m_FileID': 0, 'm_PathID': page['pathId']}):
                    raise ValueError('Unreviewed Spine atlas asset')
                covered_materials.update(p['m_PathID'] for p in atlas.get('materials', [])
                                         if p['m_FileID'] == 0)
        if not set(refs).issubset(covered_materials):
            raise ValueError('Spine material is not linked to reviewed atlas asset')


def prepare(source_dir, output_dir, profile, capabilities, catalog_path):
    import UnityPy
    from PIL import Image

    if profile.get('schemaVersion') != 1:
        raise ValueError('Unsupported compatibility profile')
    # Require results from the active Vulkan route, including the target format.
    physical = capabilities.get('maxImageDimension2D')
    extent = capabilities.get('rgba8SampledMaxExtent2D')
    if (type(physical) is not int or physical <= 0 or not isinstance(extent, list)
            or len(extent) != 2 or any(type(x) is not int or x <= 0 for x in extent)):
        raise ValueError('A current Vulkan 2D limit and sampled RGBA8 format extent are required')
    limit = min(physical, *extent)
    source_dir, output_dir = source_dir.resolve(), output_dir.resolve()
    if output_dir == source_dir or source_dir in output_dir.parents:
        raise ValueError('Output must be outside the original resource directory')
    if output_dir.exists() and any(output_dir.iterdir()):
        raise ValueError('Output directory must be empty; existing overrides are never overwritten')
    catalog_spec = profile['catalog']
    if digest(catalog_path) != catalog_spec['sourceSha256']:
        raise ValueError('Original Addressables catalog does not match the reviewed profile')
    catalog = json.loads(catalog_path.read_text(encoding='utf-8-sig'))
    if (catalog_spec['name'] != 'catalog.json' or profile['bundleDirectory'] != 'StandaloneWindows64'):
        raise ValueError('Unsupported Addressables directory layout')
    # Validate all hashes before generating any override.
    for bundle in profile['bundles']:
        name = bundle['name']
        if Path(name).name != name or '/' in name or '\\' in name:
            raise ValueError('Profile bundle names must be simple file names')
        if digest(source_dir / name) != bundle['sourceSha256']:
            raise ValueError(f'Original bundle does not match reviewed profile: {name}')

    results = []
    overrides = {}
    output_dir.mkdir(parents=True, exist_ok=True)
    for bundle in profile['bundles']:
        source = source_dir / bundle['name']
        env = UnityPy.load(str(source))
        original = {(o.assets_file.name, o.path_id): o.get_raw_data() for o in env.objects}
        changed = {}
        for expected in bundle['textures']:
            candidates = [o for o in env.objects if o.type.name == 'Texture2D'
                          and o.read().m_Name == expected['name']]
            if len(candidates) != 1:
                raise ValueError(f"Expected one texture: {expected['name']}")
            obj = candidates[0]
            data = obj.read()
            if [data.m_Width, data.m_Height, data.m_MipCount, data.m_TextureFormat] != expected['description']:
                raise ValueError(f'Unreviewed texture description: {data.m_Name}')
            texture_limit = limit
            if 'rgbaFallbackMaxDimension' in expected:
                budget_limit = expected['rgbaFallbackMaxDimension']
                if type(budget_limit) is not int or budget_limit <= 0:
                    raise ValueError('Invalid reviewed fallback dimension')
                if type(capabilities.get('textureCompressionBC')) is not bool:
                    raise ValueError('Current route BC compression capability is required')
                if capabilities['textureCompressionBC'] is False:
                    texture_limit = min(texture_limit, budget_limit)
            width, height = image_size(data.m_Width, data.m_Height, texture_limit)
            if (width, height) == (data.m_Width, data.m_Height):
                continue
            objects = obj.assets_file.objects
            trees = {pid: other.read_typetree() for pid, other in objects.items()}
            validate_texture_references(obj, objects, trees, expected, bundle['assetBundleIndexPathId'])
            record = {'name': data.m_Name, 'pathId': obj.path_id,
                      'before': [data.m_Width, data.m_Height], 'after': [width, height],
                      'format': 'RGBA32', 'mips': 1,
                      'referenceKind': expected.get('referenceKind', 'rawImage')}
            with data.image as original_image:
                image = original_image.resize((width, height), Image.Resampling.LANCZOS)
            data.set_image(image, target_format=4, mipmap_count=1)
            data.save()
            image.close()
            changed[(obj.assets_file.name, obj.path_id)] = record

        if not changed:
            results.append({'name': source.name, 'action': 'original', 'sourceSha256': bundle['sourceSha256']})
            continue
        output = output_dir / profile['bundleDirectory'] / source.name
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('xb') as stream:
            stream.write(env.file.save(packer='lz4'))
        del env
        verified = UnityPy.load(str(output))
        actual = {(o.assets_file.name, o.path_id): o for o in verified.objects}
        if original.keys() != actual.keys():
            raise ValueError('Object identities changed')
        for key, raw in original.items():
            if key not in changed:
                if actual[key].get_raw_data() != raw:
                    raise ValueError(f'Unrelated object changed: {key}')
            else:
                texture = actual[key].read()
                with texture.image as decoded:
                    decoded_size = decoded.size
                if ([texture.m_Width, texture.m_Height] != changed[key]['after']
                        or texture.m_TextureFormat != 4 or texture.m_MipCount != 1
                        or decoded_size != tuple(changed[key]['after'])):
                    raise ValueError('Texture override failed verification')
        results.append({'name': source.name, 'action': 'override',
                        'sourceSha256': bundle['sourceSha256'], 'outputSha256': digest(output),
                        'objectsVerified': len(original), 'textures': list(changed.values())})
        overrides[source.name] = {'sourceCrc': unityfs_crc(source), 'sourceSize': source.stat().st_size,
                                  'outputCrc': unityfs_crc(output), 'outputSize': output.stat().st_size,
                                  'outputSha256': digest(output)}
    updated, catalog_updates = patch_catalog(catalog, overrides)
    catalog_output = output_dir / catalog_spec['name']
    with catalog_output.open('x', encoding='utf8') as stream:
        json.dump(updated, stream, ensure_ascii=False, separators=(',', ':'))
    manifest = {'schemaVersion': 1, 'profile': profile['id'], 'sampledTextureLimit': limit,
                'd3dFeatureLevelChange': False, 'requiresDeviceValidation': True, 'bundles': results,
                'catalog': {'name': catalog_spec['name'], 'sourceSha256': digest(catalog_path),
                            'outputSha256': digest(catalog_output), 'updates': catalog_updates,
                            'crcValidationEnabled': True}}
    with (output_dir / 'compatibility-manifest.json').open('x', encoding='utf8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--capabilities', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True, help='Original matching Addressables catalog.json')
    args = parser.parse_args()
    try:
        result = prepare(args.source_dir, args.output_dir,
                         json.loads(args.profile.read_text(encoding='utf8')),
                         json.loads(args.capabilities.read_text(encoding='utf8')), args.catalog)
    except (ValueError, OSError, ImportError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
