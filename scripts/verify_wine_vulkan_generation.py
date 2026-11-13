#!/usr/bin/env python3
"""Regenerate Wine Vulkan outputs in a sparse scratch tree and compare bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

OUTPUTS = ('include/wine/vulkan.h', 'dlls/winevulkan/vulkan_thunks.h',
           'dlls/winevulkan/vulkan_thunks.c', 'dlls/winevulkan/loader_thunks.h',
           'dlls/winevulkan/loader_thunks.c', 'dlls/winevulkan/winevulkan.json',
           'dlls/winevulkan/winevulkan.spec', 'dlls/vulkan-1/vulkan-1.spec')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(source, work):
    source = source.resolve()
    work = work.resolve()
    generator = source / 'dlls/winevulkan/make_vulkan'
    xml = source / 'dlls/winevulkan/vk.xml'
    video = source / 'dlls/winevulkan/video.xml'
    wine_xml = source / 'dlls/winevulkan/winevk.xml'
    version = re.search(r'^VK_XML_VERSION = "([^"]+)"', generator.read_text(), re.M)[1]
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vulkan-regenerate-', dir=work) as temp:
        root = Path(temp)
        for name in OUTPUTS:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
        script = root / 'dlls/winevulkan/make_vulkan'
        script.write_bytes(generator.read_bytes())
        script.with_name('winevk.xml').write_bytes(wine_xml.read_bytes())
        generated = subprocess.run([sys.executable, str(script), '-x', str(xml), '-X', str(video)],
                                   capture_output=True, timeout=90)
        if generated.returncode:
            raise ValueError('Wine Vulkan generator failed: ' + generated.stderr.decode(errors='replace')[-2000:])
        result = {'xmlVersion': version, 'inputs': {
            'dlls/winevulkan/make_vulkan': sha(generator),
            'dlls/winevulkan/vk.xml': sha(xml), 'dlls/winevulkan/video.xml': sha(video),
            'dlls/winevulkan/winevk.xml': sha(wine_xml)},
            'outputs': {name: {'sourceSha256': sha(source / name),
                               'regeneratedSha256': sha(root / name)} for name in OUTPUTS}}
        different = [name for name, values in result['outputs'].items()
                     if values['sourceSha256'] != values['regeneratedSha256']]
        if different:
            raise ValueError('Wine Vulkan generated source differs: ' + ', '.join(different))
        result['allOutputsEqual'] = True
        return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', required=True, type=Path)
    p.add_argument('--work-dir', required=True, type=Path)
    p.add_argument('--report', type=Path)
    a = p.parse_args()
    result = verify(a.source, a.work_dir)
    if a.report:
        a.report.write_text(json.dumps(result, indent=2) + '\n')
    print('Wine Vulkan: all eight regenerated outputs match effective source')


if __name__ == '__main__':
    main()
