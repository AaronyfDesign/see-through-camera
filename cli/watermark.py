#!/usr/bin/env python3
"""watermark.py — Nikon body-screen watermark card (S11, docs/s11-watermark-plan.md).

W-M1 scope: single-body pipeline, profile-driven from day one.
  photo EXIF -> orientation normalize -> blurred bg canvas -> body image
  -> photo under a square-cornered LCD window -> two centered text lines
  -> JPEG out.

Discipline (inherited from S10): missing EXIF fields are a hard error, never
silently defaulted. Unknown body model -> error listing available profiles.
"""
import argparse
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parent.parent
PROFILES_DIR = ROOT / 'body_profiles'
OUT_DEFAULT = ROOT / 'output'

CANVAS_LONG = 1350                       # long edge; short edge follows the
                                         # photo's own aspect ratio (portrait
                                         # stays portrait, landscape stays
                                         # landscape)
BODY_WIDTH_FRAC = 0.80                   # body width vs canvas width
BODY_TOP_FRAC = 0.065
BG_BLUR_FRAC = 0.007                     # blur radius vs canvas width
BG_DARKEN = 0.72
SCREEN_BRIGHTEN = 1.06
LINE1_SIZE = 34                          # model · lens (sans)
LINE2_SIZE = 30                          # focal · shutter · aperture · ISO (mono)
TEXT_COLOR = (255, 255, 255, 235)
GAP_LINE = 52

FONT_SANS = '/System/Library/Fonts/Helvetica.ttc'
FONT_MONO = '/System/Library/Fonts/Menlo.ttc'


# ---------- EXIF (JPEG APP1 via Pillow; field semantics = S10 exif_info) ----

def read_exif(img):
    base = img.getexif()
    exif_ifd = base.get_ifd(0x8769)

    def need(tag, ifd, name):
        v = ifd.get(tag)
        if v is None:
            raise SystemExit(f'error: EXIF {name} missing — refusing to guess')
        return v

    def rat(v):  # Pillow returns IFDRational for rational tags
        return float(v)

    model = str(need(272, base, 'Model')).strip()
    focal = rat(need(37386, exif_ifd, 'FocalLength'))
    fnum = rat(need(33437, exif_ifd, 'FNumber'))
    expo = rat(need(33434, exif_ifd, 'ExposureTime'))
    iso = int(need(34855, exif_ifd, 'ISO'))
    orientation = int(base.get(274, 1))
    lens = str(exif_ifd.get(42036, '')).strip()  # best-effort (P1 field)
    return {'model': model, 'focal': focal, 'fnum': fnum, 'expo': expo,
            'iso': iso, 'orientation': orientation, 'lens': lens}


def fmt_shutter(sec: float) -> str:
    if sec >= 1:
        return f'{sec:g} s'
    denom = round(1 / sec)
    # snap to nearest common-ish value to avoid 1/17-style artifacts of
    # rational roundoff (S10: values come from the file, display snaps).
    return f'1/{denom} s'


# ---------- profiles ------------------------------------------------------

def load_profile(key_or_auto, model):
    profiles = {}
    for d in sorted(PROFILES_DIR.iterdir()):
        pj = d / 'profile.json'
        if d.is_dir() and pj.exists():
            profiles[d.name] = json.loads(pj.read_text())
    if not profiles:
        raise SystemExit('error: no body profiles found in body_profiles/')
    if key_or_auto != 'auto':
        if key_or_auto not in profiles:
            raise SystemExit(f'error: unknown body "{key_or_auto}"; '
                             f'available: {", ".join(profiles)}')
        return key_or_auto, profiles[key_or_auto]
    for key, p in profiles.items():
        if model in p.get('aliases', []):
            return key, p
    raise SystemExit(f'error: no profile matches model "{model}"; '
                     f'available: {", ".join(profiles)}')


def profile_png(profiles_dir, key, prof):
    p = Path(prof['png'])
    return p if p.is_absolute() else profiles_dir / key / p


# ---------- pipeline ------------------------------------------------------

def cover(img, w, h):
    s = max(w / img.width, h / img.height)
    return img.resize((round(img.width * s), round(img.height * s)),
                      Image.LANCZOS)


def build(photo_path, body_key, out_path, focal_mode='actual', debug=False,
          colorway=None, *, screen_quad=None):
    """Render a card; an optional in-memory quad never changes the profile.

    Return the actual layout for candidate annotations and measurements.
    Window bounds are edge coordinates: right/bottom are exclusive.
    """
    photo = Image.open(photo_path)
    exif = read_exif(photo)
    body_key, prof = load_profile(body_key, exif['model'])
    if colorway:
        cw = prof.get('colorways', {}).get(colorway)
        if not cw:
            opts = ', '.join(prof.get('colorways', {})) or 'none'
            raise SystemExit(f'error: no colorway "{colorway}" for {body_key}; '
                             f'available: {opts}')
        prof = {**prof, 'png': cw['png']}
        if cw.get('screen_quad'):
            prof['screen_quad'] = cw['screen_quad']
    prof = {**prof, 'png': profile_png(PROFILES_DIR, body_key, prof)}
    if screen_quad is not None:
        prof['screen_quad'] = screen_quad

    photo = ImageOps.exif_transpose(photo).convert('RGB')  # orientation -> 1
    # canvas mirrors the photo's aspect ratio exactly
    if photo.width >= photo.height:
        canvas_w = CANVAS_LONG
        canvas_h = max(1, round(CANVAS_LONG * photo.height / photo.width))
    else:
        canvas_h = CANVAS_LONG
        canvas_w = max(1, round(CANVAS_LONG * photo.width / photo.height))
    canvas = Image.new('RGB', (canvas_w, canvas_h))

    # 1. background: cover + blur + darken
    bg = cover(photo, canvas_w, canvas_h)
    bg = bg.crop(((bg.width - canvas_w) // 2, (bg.height - canvas_h) // 2,
                  (bg.width - canvas_w) // 2 + canvas_w,
                  (bg.height - canvas_h) // 2 + canvas_h))
    bg = bg.filter(ImageFilter.GaussianBlur(canvas_w * BG_BLUR_FRAC))
    bg = ImageEnhance.Brightness(bg).enhance(BG_DARKEN)
    canvas.paste(bg, (0, 0))

    # 2+3. layer sandwich: photo (middle) UNDER the body (top). The body
    #      gets a transparent window punched at the screen quad at runtime,
    #      so the bezel naturally masks any paste overflow; the punched
    #      window is EXACTLY the screen quad (square corners), and the photo
    #      fills it completely — no more, no less.
    body = Image.open(prof['png']).convert('RGBA')
    if prof.get('gray'):  # e.g. D850 placeholder: low-contrast gray look
        a = body.getchannel('A')
        rgb = body.convert('RGB')
        rgb = ImageEnhance.Color(rgb).enhance(0.25)
        rgb = ImageEnhance.Contrast(rgb).enhance(0.72)
        rgb = ImageEnhance.Brightness(rgb).enhance(0.96)
        body = Image.merge('RGBA', (*rgb.split(), a))
    qx, qy = zip(*prof['screen_quad'])
    ql, qt, qr, qb = min(qx), min(qy), max(qx), max(qy)
    window_quad = [[ql, qt], [qr, qt], [qr, qb], [ql, qb]]
    # window: EXACTLY the screen quad, square corners — the photo fills
    # the cut-out completely: no overflow, no unlit ring, no rounding.
    alpha = body.getchannel('A')
    ImageDraw.Draw(alpha).rectangle(
        [ql, qt, qr - 1, qb - 1], fill=0)
    body.putalpha(alpha)
    # photo rect: the quad plus a hair of anti-seam bleed, clamped to the
    # body silhouette — the bleed sits under the opaque bezel and is never
    # visible; it only kills subpixel seams between paste and window edge.
    bbox = alpha.point(lambda v: 255 if v > 10 else 0).getbbox()
    exp = 2
    el = min(exp, ql - bbox[0]); et = min(exp, qt - bbox[1])
    er = min(exp, bbox[2] - 1 - qr); eb = min(exp, bbox[3] - 1 - qb)
    quad = [[ql - el, qt - et], [qr + er, qt - et],
            [qr + er, qb + eb], [ql - el, qb + eb]]
    # portrait canvas: stand the body upright too (rotate visual-CCW so the
    # grip lands at the top — vertical-shooting habit), and rotate the
    # (expanded) paste quad into that space: (x, y) -> (y, orig_w - x)
    if canvas_h >= canvas_w:
        bw0 = body.width
        body = body.transpose(Image.ROTATE_90)
        quad = [[y, bw0 - x] for (x, y) in quad]
        window_quad = [[y, bw0 - x] for (x, y) in window_quad]
    orig_w, orig_h = body.size
    # fit both axes: 80% width cap, plus height cap reserving the text zone
    top_margin = round(canvas_h * BODY_TOP_FRAC)
    text_zone = 56 + GAP_LINE + LINE2_SIZE + 30
    s_body = min(canvas_w * BODY_WIDTH_FRAC / orig_w,
                 (canvas_h - top_margin - text_zone) / body.height)
    bw = round(orig_w * s_body)
    bh = round(body.height * s_body)
    body = body.resize((bw, bh), Image.LANCZOS)
    bx = (canvas_w - bw) // 2
    # always centered: horizontally on the canvas, vertically between the
    # top margin and the caption zone
    by = top_margin + (canvas_h - top_margin - text_zone - bh) // 2

    # middle layer: screen paste, cover-crop, never stretch. Per-body zoom
    # falls out of the quad size automatically.
    px, py = zip(*quad)
    s = bw / orig_w  # render scale; MUST reference pre-resize width
    left, top = min(px) * s + bx, min(py) * s + by
    right, bottom = max(px) * s + bx, max(py) * s + by
    qw, qh = round(right - left), round(bottom - top)
    screen = cover(photo, qw, qh)
    screen = screen.crop(((screen.width - qw) // 2, (screen.height - qh) // 2,
                          (screen.width - qw) // 2 + qw,
                          (screen.height - qh) // 2 + qh))
    screen = ImageEnhance.Brightness(screen).enhance(SCREEN_BRIGHTEN)
    mask = Image.new('L', (qw, qh), 0)
    ImageDraw.Draw(mask).rectangle(
        [0, 0, qw - 1, qh - 1], fill=255)  # square corners: the body's
    canvas.paste(screen, (round(left), round(top)), mask)  # bezel masks them
    # top layer: body with the punched window
    canvas.paste(body, (bx, by), body)

    # 4. text lines, centered below body
    focal = (exif['focal'] if focal_mode == 'actual'
             else round(exif['focal'] * prof.get('crop_factor', 1.0)))
    lens_part = f' · {exif["lens"]}' if exif['lens'] else ''
    line1 = f'{exif["model"]}{lens_part}'
    line2 = (f'{focal:g} mm · {fmt_shutter(exif["expo"])} · '
             f'f/{exif["fnum"]:g} · ISO {exif["iso"]}')
    draw = ImageDraw.Draw(canvas, 'RGBA')
    y = canvas_h - 24 - LINE2_SIZE - GAP_LINE  # pinned to card bottom
    max_text_w = canvas_w - 48
    for text, size, font_path in ((line1, LINE1_SIZE, FONT_SANS),
                                  (line2, LINE2_SIZE, FONT_MONO)):
        size_fit = size
        font = ImageFont.truetype(font_path, size_fit)
        while draw.textlength(text, font=font) > max_text_w and size_fit > 14:
            size_fit -= 1
            font = ImageFont.truetype(font_path, size_fit)
        tw = draw.textlength(text, font=font)
        draw.text(((canvas_w - tw) / 2, y), text, font=font,
                  fill=TEXT_COLOR)
        y += GAP_LINE

    # The requested cut-out follows the body's two actual axis scales.
    # Existing PNG transparency can also remain outside this rectangle;
    # neither that transparency nor the bleed is part of this annotation.
    wx, wy = zip(*window_quad)
    window_rect = [min(wx) * bw / orig_w + bx,
                   min(wy) * bh / orig_h + by,
                   max(wx) * bw / orig_w + bx,
                   max(wy) * bh / orig_h + by]

    if debug:  # quad overlay for calibration (saved separately, never
               # clobbering the clean render)
        dbg = canvas.copy()
        pixel_bounds = [math.floor(window_rect[0]), math.floor(window_rect[1]),
                        math.ceil(window_rect[2]) - 1, math.ceil(window_rect[3]) - 1]
        ImageDraw.Draw(dbg).rectangle(pixel_bounds,
                                      outline=(255, 0, 0), width=3)
        dbg_path = out_path.with_name(out_path.stem + '_debug.jpg')
        dbg_path.parent.mkdir(parents=True, exist_ok=True)
        dbg.save(dbg_path, quality=92)
        print(f'debug -> {dbg_path}')
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path, quality=93)
    print(f'{exif["model"]} | {line1}\n{line2}\n-> {out_path}')
    return {'canvas_size': [canvas_w, canvas_h],
            'body_rect': [bx, by, bx + bw, by + bh],
            'window_rect': window_rect,
            'paste_rect': [left, top, right, bottom],
            'screen_quad': prof['screen_quad']}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('-o', '--output', type=Path, default=None)
    ap.add_argument('--body', default='auto',
                    help='auto | profile key (see body_profiles/)')
    ap.add_argument('--focal', choices=['actual', 'eq35'], default='actual')
    ap.add_argument('--colorway', default=None,
                    help='optional colorway key (e.g. black) if the profile '
                         'defines one')
    ap.add_argument('--debug', action='store_true',
                    help='also save a quad-outline overlay for calibration')
    a = ap.parse_args()
    out = a.output or OUT_DEFAULT / 's11' / f'{a.input.stem}_wm.jpg'
    build(a.input, a.body, out, a.focal, a.debug, a.colorway)


if __name__ == '__main__':
    main()
