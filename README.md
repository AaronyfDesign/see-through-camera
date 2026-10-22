# see-through-camera

English | [中文](README.zh-CN.md)

Put your photo back where it was taken — composited into the rear LCD of a Nikon camera body, with the EXIF capture params (focal length · shutter · aperture · ISO) rendered below. A single-purpose, client-side "back-screen card" generator.

![sample](docs/sample-card.jpg)

## Highlights

- **Back-screen card** — your photo seated in the camera's rear screen, straight corners, blurred original as the backdrop
- **Orientation-aware** — portrait in, portrait out; landscape in, landscape out; the body stands upright for portrait shots (grip up)
- **EXIF-driven** — model, lens, focal length, shutter, aperture and ISO are read from the photo itself; no manual typing
- **Body profile system** — each camera body is a folder: one official product PNG + a calibrated LCD quad
- **Two ways to run** — Python CLI now, pure in-browser Canvas version on GitHub Pages (photos never leave your device)

## Repository layout

```
body_profiles/<body>/   body.png (official product render) + profile.json (LCD quad, aliases)
cli/watermark.py        Python CLI (Pillow) — render a card from a JPEG
web/                    GitHub Pages front-end (Canvas port, in progress)
docs/                   samples and calibration notes
```

## CLI usage

```bash
pip install pillow
python3 cli/watermark.py path/to/DSC_xxxx.JPG [--body z63]
```

The body is matched automatically from the photo's EXIF model field; `--body` overrides it. Output lands in `output/` next to the photo's name. Optional `--bg soft|radial|solid` picks the background treatment (uniform blur — default, radial blur sharp behind the body, or a flat fill of the photo's average color).

## Status

All 17 bodies shipped with the repo are calibrated (body.png + profile.json
with a verified LCD quad; last cross-checked 2026-10-22):

- Nikon: Z 6III, ZR, Z 5II / Z 5II C, Z 6 / Z 6II, Z 7 / Z 7II,
  Z 8 / Z 9, Z f / Z fc, Z 5, Z 30, Z 50II
- Nikon D850
- LEGO camera (easter egg)

Adding a body = dropping a `body.png` + `profile.json` (LCD quad + EXIF
aliases) folder under `body_profiles/`; the CLI and the web app pick it up
automatically.

## Credits & trademarks

Body renders are sourced from Nikon's official product pages and remain the property of Nikon Corporation. Nikon and related names are trademarks of Nikon Corporation. This project is an independent community tool, not affiliated with or endorsed by Nikon.

## License

Apache-2.0
