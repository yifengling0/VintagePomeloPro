#!/usr/bin/env python3
"""Compose classic / Deck covers from our bundled tool illustrations (Pillow).

The original illustrations remain the editable inputs. Extend only their backdrop;
never stretch the tool itself or crop a landscape cover into a portrait capsule.
FFmpeg's librsvg decoder rasterizes the two existing SVG inputs.
"""
from io import BytesIO
from pathlib import Path
import math
import subprocess

from PIL import Image, ImageDraw

MEDIA = Path(__file__).resolve().parents[1] / 'entry/src/main/resources/base/media'
RESAMPLE = getattr(Image, 'Resampling', Image)
NAMES = ('desktop', 'explorer', 'winecfg', 'iexplore', 'control', 'taskmgr',
         'regedit', 'cmd', 'notepad', 'graphics', 'audio', 'vkd3d_triangle', 'vkd3d_gears')


def illustration(name):
    png = MEDIA / f'builtin_{name}.png'
    if png.exists():
        return Image.open(png).convert('RGB')
    svg = MEDIA / f'builtin_{name}.svg'
    data = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(svg), '-frames:v', '1',
                           '-f', 'image2pipe', '-c:v', 'png', '-'],
                          check=True, capture_output=True).stdout
    return Image.open(BytesIO(data)).convert('RGB')


def cover(source, portrait):
    width, height = (400, 600) if portrait else (600, 360)
    # Sample the backdrop alone, excluding the illustration and rounded corners.
    corners = [source.crop(box).resize((1, 1)).getpixel((0, 0)) for box in (
        (12, 12, 35, 35), (source.width-35, 12, source.width-12, 35),
        (12, source.height-35, 35, source.height-12),
        (source.width-35, source.height-35, source.width-12, source.height-12))]
    field = Image.new('RGB', (2, 2)); field.putdata(corners)
    canvas = field.resize((width, height), RESAMPLE.BILINEAR)
    # Keep the original illustration's aspect and generous safe area at every ratio.
    scale = (width / source.width) if portrait else (height / source.height)
    art = source.resize((round(source.width*scale), round(source.height*scale)), RESAMPLE.LANCZOS)
    left = (width-art.width)//2
    top = round(height*.19) if portrait else 0
    # Feather the backdrop into the extended field; the central tool stays opaque.
    mask = Image.new('L', art.size)
    pixels = mask.load()
    feather = 36
    for y in range(art.height):
        for x in range(art.width):
            distance = min(x, y, art.width-1-x, art.height-1-y)
            t = min(1., distance/feather)
            pixels[x, y] = round(255*t*t*(3-2*t))
    canvas.paste(art, (left, top), mask)
    # The taller capsule has its own lower composition, continuing the flowing
    # backdrop of the source art instead of leaving letterbox bars.
    if portrait:
        waves = Image.new('RGBA', canvas.size)
        draw = ImageDraw.Draw(waves)
        for index in range(6):
            points = [(x, round(height*.83 + index*12 + 21*math.sin(x/width*4.5+index*.23)))
                      for x in range(-4, width+5, 4)]
            draw.line(points, fill=(255, 255, 255, 32-index*3), width=1)
        canvas = Image.alpha_composite(canvas.convert('RGBA'), waves).convert('RGB')
    return canvas


def main():
    for name in NAMES:
        original = illustration(name)
        for portrait in (False, True):
            layout = 'portrait' if portrait else 'landscape'
            path = MEDIA / f'builtin_{name}_{layout}.png'
            cover(original, portrait).save(path, optimize=True)
            print(path.name)


if __name__ == '__main__':
    main()
