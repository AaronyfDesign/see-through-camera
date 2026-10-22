#!/usr/bin/env python3
"""Sync the supported-body list in the READMEs from body_profiles/.

The table between the BODY-LIST markers in README.md and
README.zh-CN.md is generated — never edit it by hand.

Usage:
  scripts/gen_body_list.py           # rewrite the marked block in place
  scripts/gen_body_list.py --check   # exit 1 if the checked-in block is stale
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MARK_START = '<!-- BODY-LIST:START -->'
MARK_END = '<!-- BODY-LIST:END -->'


def collect():
    """One row per body dir: (key, EXIF aliases, quad-ok)."""
    rows = []
    for d in sorted((ROOT / 'body_profiles').iterdir()):
        prof = d / 'profile.json'
        if not d.is_dir() or not prof.exists():
            continue
        p = json.loads(prof.read_text())
        q = p.get('screen_quad')
        quad_ok = (isinstance(q, list) and len(q) == 4 and all(
            isinstance(pt, list) and len(pt) == 2 for pt in q))
        aliases = p.get('aliases') or []
        rows.append((d.name,
                     ', '.join(f'`{a}`' for a in aliases) or '—',
                     '✓' if quad_ok else '✗'))
    return rows


def render(rows):
    lines = ['| body | EXIF alias | LCD quad |', '| --- | --- | --- |']
    lines += [f'| {k} | {a} | {s} |' for k, a, s in rows]
    return '\n'.join(lines)


def patch(text, block, name):
    if MARK_START not in text or MARK_END not in text:
        raise SystemExit(f'{name}: BODY-LIST markers missing')
    i = text.index(MARK_START) + len(MARK_START)
    j = text.index(MARK_END)
    return text[:i] + '\n' + block + '\n' + text[j:]


def main():
    block = render(collect())
    stale = False
    for name in ('README.md', 'README.zh-CN.md'):
        f = ROOT / name
        text = f.read_text()
        new = patch(text, block, name)
        if new != text:
            if '--check' in sys.argv:
                print(f'{name}: stale')
                stale = True
            else:
                f.write_text(new)
                print(f'{name}: updated')
    if stale:
        sys.exit(1)


if __name__ == '__main__':
    main()
