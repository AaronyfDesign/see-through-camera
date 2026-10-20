#!/usr/bin/env python3
"""sheet.py — contact sheet of candidate body PNGs for asset picking (S11 W2).

Usage: sheet.py <dir-with-pngs> <out.jpg>
Grid of thumbnails labeled with filename stems; transparent areas shown on
mid-gray so silhouettes are visible. Scratches the "which of the 37 product
PNGs is the back view" problem without opening files one by one.
"""
import argparse
import contextlib
import hashlib
import io
import json
import math
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

CELL = 300
PAD = 8
COLS = 6


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    files = sorted(p for p in src.iterdir() if p.suffix.lower() == '.png')
    font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 16)
    rows = (len(files) + COLS - 1) // COLS
    sheet = Image.new('RGB', (COLS * (CELL + PAD) + PAD,
                              rows * (CELL + PAD + 22) + PAD), (128, 128, 128))
    d = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        img = Image.open(f).convert('RGBA')
        s = min((CELL - 2) / img.width, (CELL - 2) / img.height)
        img = img.resize((max(1, round(img.width * s)),
                          max(1, round(img.height * s))), Image.LANCZOS)
        cx = PAD + (i % COLS) * (CELL + PAD)
        cy = PAD + (i // COLS) * (CELL + PAD + 22)
        # checker-ish neutral backdrop for alpha
        d.rectangle([cx, cy, cx + CELL, cy + CELL], fill=(150, 150, 150))
        sheet.paste(img, (cx + (CELL - img.width) // 2,
                          cy + (CELL - img.height) // 2), img)
        d.text((cx + 2, cy + CELL + 2), f.stem[:22], font=font, fill=(0, 0, 0))
    sheet.save(out, quality=88)
    print(f'{len(files)} thumbs -> {out}')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inset_quad(quad, percent):
    """Inset EACH side by a fraction of its original axis, half-up to pixels."""
    if not math.isfinite(percent) or not 0 <= percent < 50:
        raise ValueError('inset must be finite and in [0, 50) percent per side')
    if len(quad) != 4 or any(len(p) != 2 for p in quad):
        raise ValueError('expected four 2D corners')
    xs, ys = zip(*quad)
    left, top, right, bottom = min(xs), min(ys), max(xs), max(ys)
    if set(map(tuple, quad)) != {(left, top), (right, top),
                                (right, bottom), (left, bottom)}:
        raise ValueError('candidate export only supports axis-aligned quads')
    if any(int(v) != v for p in quad for v in p):
        raise ValueError('source quad must use integer pixel edge coordinates')
    dx = math.floor((right - left) * percent / 100 + 0.5)
    dy = math.floor((bottom - top) * percent / 100 + 0.5)
    if right - left <= 2 * dx or bottom - top <= 2 * dy:
        raise ValueError('inset collapses the window')
    return [[left + dx, top + dy], [right - dx, top + dy],
            [right - dx, bottom - dy], [left + dx, bottom - dy]], [dx, dy]


def dominant_run(values, gap=3):
    """Longest run, bridging at most gap pixels for dark test-grid lines."""
    import numpy as np

    edges = np.diff(np.r_[False, values, False].astype(int))
    starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
    merged = []
    for start, end in zip(starts, ends):
        if merged and start - merged[-1][1] <= gap:
            merged[-1][1] = int(end)
        else:
            merged.append([int(start), int(end)])
    return max(merged, key=lambda run: run[1] - run[0]) if merged else None


def measure_bright_window(card, layout):
    """Measure a bright-grid card, not whether a quad is the correct LCD edge.

    Search across the full card width and the body height band. No expected
    quad is used to select pixels. Compare exclusive edges only afterwards.
    """
    import numpy as np

    bright = np.asarray(card.convert('L')) > 200
    _, by, _, bottom = layout['body_rect']
    bright[:by] = False
    bright[bottom:] = False
    xr = dominant_run(bright.sum(axis=0) >= 40)
    yr = dominant_run(bright.sum(axis=1) >= 40)
    measured = [xr[0], yr[0], xr[1], yr[1]] if xr and yr else None
    expected = layout['window_rect']
    delta = ([round(a - b, 4) for a, b in zip(measured, expected)]
             if measured else None)
    return {'expected_window_rect': expected, 'measured_bright_rect': measured,
            'edge_delta_px': delta,
            'max_abs_edge_delta_px': max(map(abs, delta)) if delta else None,
            'within_7px': all(abs(d) <= 7 for d in delta) if delta else False}


def save_quad_overlay(image, rect, path):
    overlay = image.copy()
    # The drawing API includes its last pixel; geometric right/bottom do not.
    pixel_bounds = [math.floor(rect[0]), math.floor(rect[1]),
                    math.ceil(rect[2]) - 1, math.ceil(rect[3]) - 1]
    ImageDraw.Draw(overlay).rectangle(pixel_bounds, outline=(255, 0, 0), width=3)
    overlay.save(path)
    return overlay


def comparison_grid(items, path, cols=2):
    """A full-resolution grid: never shrink the supplied panels."""
    pad, header = 24, 144
    width = max(image.width for image, _ in items)
    height = max(image.height for image, _ in items)
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (width + pad) + pad,
                              rows * (height + header + pad) + pad), '#eeeeea')
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 22)
    for i, (image, lines) in enumerate(items):
        x, y = pad + i % cols * (width + pad), pad + i // cols * (height + header + pad)
        for j, text in enumerate(lines):
            draw.text((x + 8, y + 6 + j * 31), text, fill='#171717', font=font)
        sheet.paste(image, (x, y + header))
    sheet.save(path, quality=95, subsampling=0)
    return list(sheet.size)


def export_candidates(photo, out, insets, excluded):
    """Read-only profile export through the production renderer, no PNG edits."""
    import numpy as np
    import watermark

    photo, out = photo.resolve(), out.resolve()
    if out.exists():
        raise ValueError(f'output already exists; choose a new directory: {out}')
    # Validate the input and all variants before writing any artifacts.
    with Image.open(photo) as image:
        exif = watermark.read_exif(image)
    if not insets or len(set(insets)) != len(insets):
        raise ValueError('insets must be nonempty and unique')
    if len(insets) > 26:
        raise ValueError('at most 26 labeled candidates are supported')
    variants = []
    for profile_path in sorted(watermark.PROFILES_DIR.glob('*/profile.json')):
        key = profile_path.parent.name
        if key in excluded:
            continue
        prof = json.loads(profile_path.read_text())
        for color, cw in [(None, {})] + list(prof.get('colorways', {}).items()):
            effective = {**prof, **cw}
            png = watermark.profile_png(watermark.PROFILES_DIR, key, effective)
            quad = effective['screen_quad']
            for percent in insets:
                inset_quad(quad, percent)
            with Image.open(png) as image:
                xs, ys = zip(*quad)
                if not (0 <= min(xs) < max(xs) <= image.width and
                        0 <= min(ys) < max(ys) <= image.height):
                    raise ValueError(f'quad outside image: {key}/{color}')
            variants.append((key, color, png, quad))
    if not variants:
        raise ValueError('no variants selected')
    protected = sorted(p for base in (watermark.PROFILES_DIR,
                                      watermark.ROOT / 'web' / 'bodies')
                       for p in base.rglob('*') if p.is_file())
    before = {str(p.relative_to(watermark.ROOT)): sha256(p) for p in protected}
    revision = subprocess.check_output(
        ['git', '-C', str(watermark.ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    out.mkdir(parents=True)
    inputs = out / 'inputs'
    inputs.mkdir()
    source_copy = inputs / photo.name
    shutil.copy2(photo, source_copy)
    # Include the exact renderer/exporter used, so reproduction does not
    # depend on later working-tree edits or a temporary helper script.
    code = out / 'reproduce'
    code.mkdir()
    for source in (Path(watermark.__file__), Path(__file__)):
        shutil.copy2(source, code / source.name)
    records, panels = [], []
    for key, color, png, original_quad in variants:
        variant = key + (f'_{color}' if color else '')
        folder = out / variant
        folder.mkdir()
        with Image.open(png) as image:
            body = image.convert('RGBA')
        alpha = np.asarray(body.getchannel('A'))
        source = Image.new('RGBA', body.size, (36, 36, 34, 255))
        source.alpha_composite(body)
        source = source.convert('RGB')
        source.save(folder / 'source_reference.png')
        xs, ys = zip(*original_quad)
        baseline_mask = np.zeros(alpha.shape, dtype=bool)
        baseline_mask[min(ys):max(ys), min(xs):max(xs)] = True
        full_items, zoom_items, source_items = [], [], []
        zoom_box = None
        for i, percent in enumerate(insets):
            label = chr(ord('A') + i)
            quad, (dx, dy) = inset_quad(original_quad, percent)
            candidate_id = f'{label}_{percent:g}pct'
            dest = folder / candidate_id
            dest.mkdir()
            card_path = dest / 'card.jpg'
            with contextlib.redirect_stdout(io.StringIO()):
                layout = watermark.build(photo, key, card_path, colorway=color,
                                         screen_quad=quad)
            with Image.open(card_path) as image:
                card = image.convert('RGB')
            if card.size != tuple(layout['canvas_size']):
                raise AssertionError('render dimensions do not match its layout')
            save_quad_overlay(card, layout['window_rect'], dest / 'card_quad.png')
            x, y = zip(*quad)
            rect = [min(x), min(y), max(x), max(y)]
            annotated_source = save_quad_overlay(source, rect, dest / 'source_quad.png')
            if zoom_box is None:
                # Fixed crop for every candidate. Compute it from the original
                # quad using the body's real independent X/Y resize scales.
                cw, ch = layout['canvas_size']
                bx, by, br, bb = layout['body_rect']
                coords = original_quad
                ow, oh = body.size
                if ch >= cw:
                    coords = [[qy, ow - qx] for qx, qy in coords]
                    ow, oh = oh, ow
                qx, qy = zip(*coords)
                bounds = [min(qx) * (br - bx) / ow + bx,
                          min(qy) * (bb - by) / oh + by,
                          max(qx) * (br - bx) / ow + bx,
                          max(qy) * (bb - by) / oh + by]
                zoom_box = [max(0, math.floor(bounds[0]) - 40),
                            max(0, math.floor(bounds[1]) - 40),
                            min(cw, math.ceil(bounds[2]) + 40),
                            min(ch, math.ceil(bounds[3]) + 40)]
            zoom = card.crop(zoom_box)
            zoom = zoom.resize((zoom.width * 2, zoom.height * 2), Image.Resampling.NEAREST)
            zoom.save(dest / 'lcd_2x.png')
            zoom_quad = [(layout['window_rect'][j] - zoom_box[j % 2]) * 2
                         for j in range(4)]
            save_quad_overlay(zoom, zoom_quad, dest / 'lcd_2x_quad.png')
            proposed_mask = np.zeros(alpha.shape, dtype=bool)
            proposed_mask[min(y):max(y), min(x):max(x)] = True
            ring = baseline_mask & ~proposed_mask
            measurement = measure_bright_window(card, layout)
            transparent_outside = int(((alpha == 0) & ring).sum())
            record = {
                'variant': variant, 'body': key, 'colorway': color,
                'candidate': label, 'inset_percent_per_side': percent,
                'source_png': str(png.relative_to(watermark.ROOT)),
                'source_png_sha256': sha256(png), 'source_size': list(body.size),
                'original_screen_quad': original_quad, 'screen_quad': quad,
                'inset_source_px_each_side_xy': [dx, dy],
                'window_size_source_px': [max(x) - min(x), max(y) - min(y)],
                'layout': layout, 'lcd_crop_card_px': zoom_box,
                'measurement': measurement,
                'retained_ring_source_pixels': int(ring.sum()),
                'retained_ring_fully_opaque_source_pixels': int(((alpha == 255) & ring).sum()),
                'transparent_source_pixels_inside_baseline_outside_candidate': transparent_outside,
                'alpha_warning': bool(transparent_outside),
                'card': str(card_path.relative_to(out)),
            }
            records.append(record)
            title = [f'{variant}  |  {label}  {percent:g}% / side',
                     f'inset x={dx}px, y={dy}px  (SOURCE pixels)',
                     f'quad [{min(x)},{min(y)}]-[{max(x)},{max(y)}]',
                     f'ALPHA=0 outside quad: {transparent_outside} src px']
            full_items.append((card, title))
            zoom_items.append((zoom, title))
            source_items.append((annotated_source, title))
        size = comparison_grid(full_items, folder / 'comparison.jpg')
        zoom_size = comparison_grid(zoom_items, folder / 'lcd_comparison.jpg')
        source_size = comparison_grid(source_items, folder / 'source_quad_comparison.jpg')
        panels.append({'variant': variant, 'comparison_size': size,
                       'lcd_comparison_size': zoom_size,
                       'source_comparison_size': source_size,
                       'comparison': f'{variant}/comparison.jpg',
                       'lcd_comparison': f'{variant}/lcd_comparison.jpg',
                       'source_comparison': f'{variant}/source_quad_comparison.jpg'})
        print(f'{variant}: {len(insets)} candidates -> {folder}', flush=True)
    after = {str(p.relative_to(watermark.ROOT)): sha256(p)
             for base in (watermark.PROFILES_DIR, watermark.ROOT / 'web' / 'bodies')
             for p in base.rglob('*') if p.is_file()}
    if before != after:
        raise RuntimeError('source assets changed during export; do not trust this pack')
    manifest = {
        'schema_version': 1,
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'base_commit': revision,
        'renderer_sha256': sha256(Path(watermark.__file__)),
        'exporter_sha256': sha256(Path(__file__)),
        'input_photo': str(source_copy.relative_to(out)),
        'input_sha256': sha256(photo), 'input_exif': exif,
        'excluded_bodies': sorted(excluded),
        'body_count': len({v[0] for v in variants}),
        'variant_count': len(variants), 'candidate_count': len(records),
        'inset_definition': 'Each side: round-half-up(original axis size * percent / 100).',
        'coordinate_convention': 'Pixel edge coordinates; right and bottom exclusive.',
        'red_frame': 'Candidate quad, NOT bleed, NOT an inferred LCD boundary. '
                     'Raster outline uses floor(left/top), ceil(right/bottom)-1.',
        'alpha_warning': 'Original PNG transparency outside a candidate is NOT restored '
                         'by shrinking the quad. ALPHA=0 counts only pixels within the '
                         'original quad but outside the candidate. These may show bleed '
                         'or background, not a preserved opaque bezel.',
        'alpha_warning_candidate_count': sum(r['alpha_warning'] for r in records),
        'source_reference_background_rgb': [36, 36, 34],
        'lcd_zoom': '2x nearest-neighbor, fixed original-quad crop plus 40 card px.',
        'measurement_method': {
            'threshold': 'Pillow grayscale > 200',
            'search_region': 'Full card width, body top to body bottom; excludes captions.',
            'projection_threshold': 'At least 40 bright pixels per row/column',
            'run_selection': 'Longest run, merge gaps <= 3 pixels for dark grid lines',
            'edge_tolerance_px': 7,
            'scope': 'Bright-grid fill versus candidate quad only; NOT LCD/bezel correctness.',
        },
        'protected_assets_unchanged': before == after,
        'protected_assets_sha256': before,
        'comparisons': panels, 'candidates': records,
    }
    # Raw measurements, including any failures, are retained without silently
    # changing thresholds or selecting a different expected rectangle.
    manifest['measurement_summary'] = {
        'within_7px_count': sum(r['measurement']['within_7px'] for r in records),
        'max_abs_edge_delta_px': max(
            (r['measurement']['max_abs_edge_delta_px'] for r in records
             if r['measurement']['max_abs_edge_delta_px'] is not None), default=None),
        'undetected_count': sum(r['measurement']['measured_bright_rect'] is None for r in records),
    }
    manifest['artifacts'] = {
        str(path.relative_to(out)): {'sha256': sha256(path), 'bytes': path.stat().st_size}
        for path in sorted(out.rglob('*')) if path.is_file()
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({key: manifest[key] for key in
                      ('body_count', 'variant_count', 'candidate_count',
                       'protected_assets_unchanged', 'measurement_summary')}, indent=2))


def candidate_main():
    parser = argparse.ArgumentParser(description='Export read-only LCD quad candidates.')
    parser.add_argument('--candidates', action='store_true', required=True)
    parser.add_argument('--photo', type=Path, required=True,
                        help='bright grid JPEG with complete EXIF, for pixel measurements')
    parser.add_argument('--output', type=Path, required=True,
                        help='a NEW directory for images and manifest.json')
    parser.add_argument('--insets', type=float, nargs='+', default=[0, 3, 5, 8],
                        help='percent of original width/height removed from EACH side')
    parser.add_argument('--exclude', nargs='*', default=['lego', 'zfc'])
    args = parser.parse_args()
    try:
        export_candidates(args.photo, args.output, args.insets, set(args.exclude))
    except (ValueError, FileNotFoundError) as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    if '--candidates' in sys.argv:
        candidate_main()
    else:
        main()
