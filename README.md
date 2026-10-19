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

The body is matched automatically from the photo's EXIF model field; `--body` overrides it. Output lands in `output/` next to the photo's name.

## Status

- [x] Z6_3 — calibrated
- [x] ZR / Z5 / Z8 / Z9 / Zf / Zfc — calibrated (LCD quad hand-read from 2× ruler overlays)

## Credits & trademarks

Body renders are sourced from Nikon's official product pages and remain the property of Nikon Corporation. Nikon and related names are trademarks of Nikon Corporation. This project is an independent community tool, not affiliated with or endorsed by Nikon.

## License

Apache-2.0
