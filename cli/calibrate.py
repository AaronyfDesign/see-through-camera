#!/usr/bin/env python3
"""Calibrate an LCD screen quad from a body render (smooth-dark run method).

The screen on a product render is a large, smooth, near-black rectangle.
Body leather has texture, buttons have bright icons — so a pixel is
screen-ish when it is dark AND locally flat. Per row we take the longest
consecutive screen-ish run; rows whose run exceeds 35% of the width form
the screen band; run start/end medians give the column bounds.

Usage: python3 calibrate.py <body_dir> [--rotate-ccw]
Writes profile.json (if valid) and calibrate_debug.png.
"""
import json
import statistics
import sys
from pathlib import Path

from PIL import Image, ImageDraw

MIN_RUN_FRAC = 0.30


def luminance_map(img):
    w, h = img.size
    px = img.load()
    lum = [[0] * w for _ in range(h)]
    for y in range(h):
        row = lum[y]
        for x in range(w):
            r, g, b = px[x, y][:3]
            row[x] = (r * 299 + g * 587 + b * 114) // 1000
    return lum, w, h


def smooth_dark_mask(lum, w, h):
    """dark AND flat in a 5x5 neighborhood (screen interior, not texture)."""
    m = [[False] * w for _ in range(h)]
    for y in range(2, h - 2):
        for x in range(2, w - 2):
            c = lum[y][x]
            if c >= 80:
                continue
            flat = True
            for dy in (-2, 0, 2):
                for dx in (-2, 0, 2):
                    if abs(lum[y + dy][x + dx] - c) > 6:
                        flat = False
                        break
                if not flat:
                    break
            m[y][x] = flat
    return m


def longest_run(bools):
    best = (0, 0, 0)  # length, start, end(inclusive)
    s = None
    for i, v in enumerate(bools + [False]):
        if v and s is None:
            s = i
        elif not v and s is not None:
            if i - s > best[0]:
                best = (i - s, s, i - 1)
            s = None
    return best


def calibrate(img):
    lum, w, h = luminance_map(img)
    m = smooth_dark_mask(lum, w, h)
    # per-row longest run of screen-ish pixels
    screen_rows = []  # (y, start, end)
    for y in range(h):
        ln, s, e = longest_run(m[y])
        if ln > w * MIN_RUN_FRAC:
            screen_rows.append((y, s, e))
    if not screen_rows:
        return None, None, m
    # largest contiguous band of screen rows (gap tolerance 6 rows)
    bands, cur = [], [screen_rows[0]]
    for item in screen_rows[1:]:
        if item[0] - cur[-1][0] <= 6:
            cur.append(item)
        else:
            bands.append(cur)
            cur = [item]
    bands.append(cur)
    band = max(bands, key=lambda b: b[-1][0] - b[0][0])
    y0, y1 = band[0][0], band[-1][0]
    starts = [s for _, s, _ in band]
    ends = [e for _, _, e in band]
    x0 = int(statistics.median(starts))
    x1 = int(statistics.median(ends))
    # refine y bounds per column within [x0, x1]
    col_screen_ys = []
    for x in range(x0, x1 + 1, 4):
        col = [y for y in range(max(0, y0 - 30), min(h, y1 + 31))
               if m[y][x]]
        if col:
            col_screen_ys.append((min(col), max(col)))
    if col_screen_ys:
        y0 = int(statistics.median(c[0] for c in col_screen_ys))
        y1 = int(statistics.median(c[1] for c in col_screen_ys))
    return (x0, y0, x1, y1), band, m


def main():
    body_dir = Path(sys.argv[1])
    rotate = '--rotate-ccw' in sys.argv
    img = Image.open(body_dir / 'body.png').convert('RGB')
    if rotate:
        img = img.transpose(Image.ROTATE_90)
    bbox, band, m = calibrate(img)
    if bbox is None:
        print('FAIL: no smooth dark screen region found')
        sys.exit(1)
    x0, y0, x1, y1 = bbox
    qw, qh = x1 - x0 + 1, y1 - y0 + 1
    print(f'quad: [[{x0}, {y0}], [{x1}, {y0}], [{x1}, {y1}], [{x0}, {y1}]]')
    print(f'size: {qw}x{qh}  aspect={qw / qh:.3f}  body={img.width}x{img.height}')
    dbg = img.copy()
    d = ImageDraw.Draw(dbg)
    d.rectangle([x0, y0, x1, y1], outline=(255, 40, 40), width=3)
    dbg.save(body_dir / 'calibrate_debug.png')
    print(f"debug -> {body_dir / 'calibrate_debug.png'}")


if __name__ == '__main__':
    main()
