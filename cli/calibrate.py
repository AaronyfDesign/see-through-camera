#!/usr/bin/env python3
"""Read an existing rectangular LCD cutout from PNG alpha, never dark glass.

Usage: calibrate.py [body_dir] --output <new-evidence-directory>
With no body_dir, inspect every profile except lego and zfc. This command
only writes evidence; it never changes a source PNG or profile.json.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent


def transparent_window(image, search_quad):
    """Find a dominant, fully transparent rectangular component inside the ROI.

    The existing quad is ONLY a search region, not a calibration reference.
    A surrounding nonzero-alpha ring proves the result was not truncated by
    that ROI. Small disconnected transparent lettering is reported separately.
    Fail rather than guess if the component is irregular, open, or ambiguous.
    """
    alpha = np.array(image.convert('RGBA').getchannel('A'))
    xs, ys = zip(*search_quad)
    l, t, r, b = min(xs), min(ys), max(xs), max(ys)
    if any(int(v) != v for v in (l, t, r, b)):
        raise ValueError('search bounds must be integer pixel edges')
    l, t, r, b = map(int, (l, t, r, b))
    if not (0 <= l < r <= image.width and 0 <= t < b <= image.height):
        raise ValueError('search bounds outside source image')
    remaining = (alpha[t:b, l:r] == 0).astype(np.uint8) * 255
    components = []
    while np.any(remaining):
        yy, xx = np.nonzero(remaining)
        mask = Image.fromarray(remaining).copy()
        ImageDraw.floodfill(mask, (int(xx[0]), int(yy[0])), 128)
        component = np.array(mask) == 128
        yy, xx = np.nonzero(component)
        bounds = [int(xx.min()) + l, int(yy.min()) + t,
                  int(xx.max()) + l + 1, int(yy.max()) + t + 1]
        pixels = int(component.sum())
        area = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
        components.append({'bounds': bounds, 'pixels': pixels,
                           'rectangular': pixels == area})
        remaining[component] = 0
    if not components:
        raise ValueError('no alpha=0 component; refusing to infer a dark-glass boundary')
    components.sort(key=lambda item: item['pixels'], reverse=True)
    window = components[0]
    if not window['rectangular']:
        raise ValueError('largest transparent component is not a complete rectangle')
    if len(components) > 1 and components[1]['pixels'] * 10 >= window['pixels']:
        raise ValueError('multiple substantial holes; manual selection required')
    left, top, right, bottom = window['bounds']
    if left == 0 or top == 0 or right == image.width or bottom == image.height:
        raise ValueError('transparent component reaches the image boundary')
    ring = np.concatenate((alpha[top - 1, left:right], alpha[bottom, left:right],
                           alpha[top:bottom, left - 1], alpha[top:bottom, right]))
    if np.any(ring == 0):
        raise ValueError('transparent component is open or truncated by the search bounds')
    quad = [[left, top], [right, top], [right, bottom], [left, bottom]]
    return {
        'screen_quad': quad, 'bounds': window['bounds'],
        'size': [right - left, bottom - top],
        'transparent_window_pixels': window['pixels'],
        'nontransparent_pixels_inside_window': int(np.count_nonzero(alpha[top:bottom, left:right])),
        'surrounding_ring_min_alpha': int(ring.min()),
        'search_quad': search_quad,
        'old_quad_nontransparent_pixels': int(np.count_nonzero(alpha[t:b, l:r])),
        'old_to_inner_ltrb_px': [left - l, top - t, r - right, b - bottom],
        'other_transparent_components': components[1:],
    }


def export_evidence(profile_dirs, output):
    if output.exists():
        raise ValueError(f'output already exists: {output}')
    records = []
    # Complete detection first; do not silently emit a partial success pack.
    for directory in profile_dirs:
        profile_path = directory / 'profile.json'
        prof = json.loads(profile_path.read_text())
        for color, cw in [(None, {})] + list(prof.get('colorways', {}).items()):
            effective = {**prof, **cw}
            png = directory / effective['png']
            with Image.open(png) as image:
                record = transparent_window(image, effective['screen_quad'])
            records.append({**record, 'body': directory.name, 'colorway': color,
                            'variant': directory.name + (f'_{color}' if color else ''),
                            'png': str(png.resolve()),
                            'png_sha256': hashlib.sha256(png.read_bytes()).hexdigest()})
    output.mkdir(parents=True)
    for record in records:
        folder = output / record['variant']
        folder.mkdir()
        with Image.open(record['png']) as image:
            body = image.convert('RGBA')
        source = Image.new('RGBA', body.size, (36, 36, 34, 255))
        source.alpha_composite(body)
        source = source.convert('RGB')
        source.save(folder / 'source.png')
        draw = ImageDraw.Draw(source)
        l, t, r, b = record['bounds']
        draw.rectangle([l, t, r - 1, b - 1], outline=(255, 0, 0), width=2)
        source.save(folder / 'inner_quad.png')
        body.getchannel('A').save(folder / 'source_alpha.png')
        print(record['variant'], record['bounds'], 'size', record['size'],
              'old erased nontransparent pixels', record['old_quad_nontransparent_pixels'])
    report = {'method': 'Largest enclosed alpha=0 component, complete rectangle required.',
              'coordinate_convention': 'Pixel edges; right and bottom exclusive.',
              'source_pngs_modified': False, 'profiles_modified': False,
              'variants': records}
    (output / 'geometry.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(records)} variants -> {output / "geometry.json"}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('body_dir', type=Path, nargs='?')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exclude', nargs='*', default=['lego', 'zfc'])
    args = parser.parse_args()
    directories = ([args.body_dir] if args.body_dir else
                   [p.parent for p in sorted((ROOT / 'body_profiles').glob('*/profile.json'))
                    if p.parent.name not in args.exclude])
    try:
        export_evidence(directories, args.output.resolve())
    except (ValueError, FileNotFoundError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
