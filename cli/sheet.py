#!/usr/bin/env python3
"""sheet.py — contact sheet of candidate body PNGs for asset picking (S11 W2).

Usage: sheet.py <dir-with-pngs> <out.jpg>
Grid of thumbnails labeled with filename stems; transparent areas shown on
mid-gray so silhouettes are visible. Scratches the "which of the 37 product
PNGs is the back view" problem without opening files one by one.
"""
import sys
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


if __name__ == '__main__':
    main()
