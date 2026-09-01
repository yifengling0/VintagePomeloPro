#!/usr/bin/env python3
"""PIL/numpy frame validators for the WineHua regression suite.

Replaces the System.Drawing validators that previously ran inside the Windows
PowerShell orchestrator. Runs standalone so the same suite works from WSL pwsh.

Exit code 0 means PASS, 1 means FAIL. The machine-readable JSON is written to
--output when given, and printed to stdout otherwise.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load_sampled_rgb(path: Path, step: int = 4) -> tuple[np.ndarray, np.ndarray, int, int]:
    """Return (sampled pixels, (x, y) coordinate grids, width, height).

    The grids carry original pixel coordinates (0, step, 2*step, ...) so
    centroids and bounds match the previous System.Drawing implementation.
    """
    with Image.open(path) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
        width, height = image.size
    xs = np.arange(0, width, step)
    ys = np.arange(0, height, step)
    xgrid, ygrid = np.meshgrid(xs, ys)
    return rgb[::step, ::step], xgrid, ygrid, width, height


def validate_rgba_quadrants(image_path: Path, step: int = 4) -> dict:
    """Four-colour quadrant topology, rotation-invariant (validator
    'rgba-quadrants-v1-rotations'). Product overlays may reuse one fixture
    colour outside the Wine window, so infer the window split from the other
    three colours and count every colour only in its expected quadrant."""
    pixels, xgrid, ygrid, width, height = load_sampled_rgb(image_path, step)
    r = pixels[..., 0].astype(np.int16)
    g = pixels[..., 1].astype(np.int16)
    b = pixels[..., 2].astype(np.int16)

    masks = {
        "red": (r > 170) & (g < 100) & (b < 120),
        "green": (g > 150) & (r < 120) & (b < 130),
        "blue": (b > 160) & (r < 130) & (g < 140),
        "yellow": (r > 170) & (g > 140) & (b < 120),
    }
    layouts = [
        {"name": "identity", "TL": "red", "TR": "green", "BL": "blue", "BR": "yellow"},
        {"name": "rotate90", "TL": "blue", "TR": "red", "BL": "yellow", "BR": "green"},
        {"name": "rotate180", "TL": "yellow", "TR": "blue", "BL": "green", "BR": "red"},
        {"name": "rotate270", "TL": "green", "TR": "yellow", "BL": "red", "BR": "blue"},
    ]
    sample_count = math.ceil(width / step) * math.ceil(height / step)
    minimum = max(80, int(sample_count * 0.003))

    raw_centroids = {}
    for name, mask in masks.items():
        count = int(mask.sum())
        raw_centroids[name] = {
            "count": count,
            "x": float(xgrid[mask].mean()) if count else -1.0,
            "y": float(ygrid[mask].mean()) if count else -1.0,
        }

    # Red/green/yellow are unique in the product chrome. The product's blue
    # background, however, can be larger than the blue fixture quadrant. Test
    # each legal rotation with the three uncontaminated centroids and choose
    # the strongest separated L-shape; this locates the Wine content split.
    candidates = []
    known_colours = ("red", "green", "yellow")
    for layout in layouts:
        colour_quadrant = {
            layout[quadrant]: quadrant for quadrant in ("TL", "TR", "BL", "BR")
        }
        left = [raw_centroids[name]["x"] for name in known_colours
                if colour_quadrant[name].endswith("L")]
        right = [raw_centroids[name]["x"] for name in known_colours
                 if colour_quadrant[name].endswith("R")]
        top = [raw_centroids[name]["y"] for name in known_colours
               if colour_quadrant[name].startswith("T")]
        bottom = [raw_centroids[name]["y"] for name in known_colours
                  if colour_quadrant[name].startswith("B")]
        if not (left and right and top and bottom):
            continue
        column_gap = min(right) - max(left)
        row_gap = min(bottom) - max(top)
        if column_gap <= width * 0.08 or row_gap <= height * 0.08:
            continue
        center_x = (max(left) + min(right)) / 2.0
        center_y = (max(top) + min(bottom)) / 2.0
        candidates.append((column_gap / width + row_gap / height,
                           layout, colour_quadrant, center_x, center_y))

    detected_transform = None
    quadrants = {}
    centroids = raw_centroids
    if candidates:
        _, layout, colour_quadrant, center_x, center_y = max(
            candidates, key=lambda item: item[0])
        detected_transform = layout["name"]
        centroids = {}
        for name, mask in masks.items():
            quadrant = colour_quadrant[name]
            expected_region = ((xgrid < center_x) if quadrant.endswith("L") else
                               (xgrid >= center_x))
            expected_region &= ((ygrid < center_y) if quadrant.startswith("T") else
                                (ygrid >= center_y))
            local_mask = mask & expected_region
            count = int(local_mask.sum())
            centroids[name] = {
                "count": count,
                "x": float(xgrid[local_mask].mean()) if count else -1.0,
                "y": float(ygrid[local_mask].mean()) if count else -1.0,
            }
            if count:
                column = "L" if centroids[name]["x"] < center_x else "R"
                row = "T" if centroids[name]["y"] < center_y else "B"
                quadrants[f"{row}{column}"] = name

    enough = all(centroids[name]["count"] >= minimum for name in masks)
    x_values = [centroids[name]["x"] for name in masks]
    y_values = [centroids[name]["y"] for name in masks]
    separated_columns = (max(x_values) - min(x_values)) > (width * 0.08)
    separated_rows = (max(y_values) - min(y_values)) > (height * 0.08)
    topology_matches = False
    if detected_transform is not None and len(quadrants) == 4:
        expected = next(layout for layout in layouts
                        if layout["name"] == detected_transform)
        topology_matches = all(
            quadrants.get(key) == expected[key] for key in ("TL", "TR", "BL", "BR"))
    passed = bool(enough and separated_columns and separated_rows and topology_matches)

    return {
        "schemaVersion": 1,
        "status": "PASS" if passed else "FAIL",
        "validator": "rgba-quadrants-v1-rotations",
        "image": str(image_path),
        "width": width,
        "height": height,
        "minimumSamplesPerColor": minimum,
        "detectedTransform": detected_transform,
        "quadrants": quadrants,
        "centroids": centroids,
        "rawColorSamples": {name: raw_centroids[name]["count"] for name in masks},
    }


def validate_d3d11_cube(image_path: Path, step: int = 4) -> dict:
    """Coloured cube with depth/background variety (validator
    'd3d11-cube-color-depth-v1')."""
    pixels, xgrid, ygrid, width, height = load_sampled_rgb(image_path, step)
    r = pixels[..., 0].astype(np.int16)
    g = pixels[..., 1].astype(np.int16)
    b = pixels[..., 2].astype(np.int16)
    maximum = np.maximum(np.maximum(r, g), b)
    minimum = np.minimum(np.minimum(r, g), b)

    dark = maximum < 55
    colored = (maximum > 100) & ((maximum - minimum) > 55)
    buckets = {
        "red": (colored & (r == maximum)).sum(),
        "green": (colored & (r != maximum) & (g == maximum)).sum(),
        "blue": (colored & (r != maximum) & (g != maximum)).sum(),
    }
    sample_count = math.ceil(width / step) * math.ceil(height / step)
    minimum_colored = max(500, int(sample_count * 0.005))
    active_buckets = sum(1 for value in buckets.values() if value > minimum_colored * 0.08)

    colored_count = int(colored.sum())
    if colored_count:
        min_x = int(xgrid[colored].min())
        max_x = int(xgrid[colored].max())
        min_y = int(ygrid[colored].min())
        max_y = int(ygrid[colored].max())
        box_width, box_height = max_x - min_x + 1, max_y - min_y + 1
    else:
        min_x = min_y = max_x = max_y = -1
        box_width = box_height = 0
    dark_count = int(dark.sum())

    passed = bool(
        colored_count >= minimum_colored
        and active_buckets >= 3
        and box_width > (width * 0.08)
        and box_height > (height * 0.08)
        and dark_count > (sample_count * 0.03)
    )

    return {
        "schemaVersion": 1,
        "status": "PASS" if passed else "FAIL",
        "validator": "d3d11-cube-color-depth-v1",
        "image": str(image_path),
        "width": width,
        "height": height,
        "coloredSamples": colored_count,
        "minimumColoredSamples": minimum_colored,
        "darkSamples": dark_count,
        "activeColorBuckets": active_buckets,
        "colorBuckets": {name: int(value) for name, value in buckets.items()},
        "coloredBounds": {"x": min_x, "y": min_y, "width": box_width, "height": box_height},
    }


VALIDATORS = {
    "rgba-quadrants": validate_rgba_quadrants,
    "d3d11-cube": validate_d3d11_cube,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", required=True, choices=sorted(VALIDATORS))
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report = VALIDATORS[args.validator](args.image)
    encoded = json.dumps(report, sort_keys=True, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
