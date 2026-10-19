/* app.js — see-through-camera, browser port of cli/watermark.py.
 *
 * Pipeline parity with the Python version:
 *   EXIF -> orientation normalize -> cover+blur+darken bg -> layer sandwich
 *   (photo middle layer, body top layer with a transparent window punched
 *   at the screen quad; portrait: rotate visual-CCW, quad remap) -> two
 *   centered text lines -> JPEG.
 *
 * Mobile compatibility:
 *   - decode via <img> + object URL (createImageBitmap+options is missing on
 *     older iOS); browsers apply EXIF orientation when drawing <img>
 *   - ctx.filter is unsupported on iOS Safari < 18: blur falls back to
 *     downscale/upscale, brightness to overlay fills (query ?legacy=1 forces
 *     the fallback on desktop for testing)
 *
 * EXIF policy: never guess. Fields that fail to parse are simply left out of
 * the card; a missing model shows a warning and the selected body's label
 * stands in on the card (the user chose that body).
 */
'use strict';

// ---------- constants (mirror cli/watermark.py) ----------
const CANVAS_LONG = 1350;
const BODY_WIDTH_FRAC = 0.80;
const BODY_TOP_FRAC = 0.065;
const BG_BLUR_FRAC = 0.007;
const BG_DARKEN = 0.72;
const SCREEN_BRIGHTEN = 1.06;
const LINE1_SIZE = 34, LINE2_SIZE = 30, GAP_LINE = 52;
const FONT_SANS = '-apple-system,"PingFang SC","Helvetica Neue",Helvetica,Arial,sans-serif';
const FONT_MONO = '"SF Mono",Menlo,Consolas,"Courier New",monospace';

// lego is the default entry (toy camera cutout, no EXIF auto-match);
// d850 renders in low-contrast gray (profile "gray":true)
const BODIES = [
  { key: 'lego', label: 'LEGO 相机', profile: 'bodies/lego/profile.json', ready: true },
  { key: 'd850', label: 'D850',   profile: 'bodies/d850/profile.json',  ready: true,
    brand: 'NIKON' },
  { key: 'z63',  label: 'Z 6III', profile: 'bodies/z63/profile.json',   ready: true,
    brand: 'NIKON' },
  { key: 'zr',   label: 'ZR',     profile: 'bodies/zr/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'z5',   label: 'Z 5',    profile: 'bodies/z5/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'z52',  label: 'Z 5II',  profile: 'bodies/z52/profile.json',   ready: true,
    brand: 'NIKON' },
  { key: 'z5iic', label: 'Z 5II C', profile: 'bodies/z5iic/profile.json', ready: true,
    hasColorways: true, brand: 'NIKON' },
  { key: 'z62',  label: 'Z 6II',  profile: 'bodies/z62/profile.json',   ready: true,
    brand: 'NIKON' },
  { key: 'z6',   label: 'Z 6',    profile: 'bodies/z6/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'z72',  label: 'Z 7II',  profile: 'bodies/z72/profile.json',   ready: true,
    brand: 'NIKON' },
  { key: 'z7',   label: 'Z 7',    profile: 'bodies/z7/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'z502', label: 'Z 50II', profile: 'bodies/z502/profile.json',  ready: true,
    brand: 'NIKON' },
  { key: 'z30',  label: 'Z 30',   profile: 'bodies/z30/profile.json',   ready: true,
    brand: 'NIKON' },
  { key: 'z8',   label: 'Z 8',    profile: 'bodies/z8/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'z9',   label: 'Z 9',    profile: 'bodies/z9/profile.json',    ready: true,
    brand: 'NIKON' },
  { key: 'zf',   label: 'Z f',    profile: 'bodies/zf/profile.json',    ready: true,
    hasColorways: true, brand: 'NIKON' },
  { key: 'zfc',  label: 'Z fc',   profile: 'bodies/zfc/profile.json',   ready: true,
    hasColorways: true, brand: 'NIKON' },
];

const FILTER_OK = (() => {
  const c = document.createElement('canvas').getContext('2d');
  return !new URLSearchParams(location.search).has('legacy') &&
         typeof c.filter === 'string' &&
         (c.filter = 'blur(1px)', c.filter === 'blur(1px)');
})();

// ---------- state ----------
const $ = (id) => document.getElementById(id);
const els = {
  drop: $('drop'), file: $('file'), previewWrap: $('previewWrap'),
  preview: $('preview'), err: $('err'), controls: $('controls'),
  bodySel: $('bodySel'), cwSeg: $('cwSeg'), repick: $('repick'), save: $('save'),
};
let state = {
  file: null,        // picked File
  img: null,         // HTMLImageElement, orientation-normalized by the browser
  params: null,      // partial: any of model/lens/focal/expo/fnum/iso
  bodyKey: 'lego',   // user-selected body (default: toy camera)
  colorway: 'silver',// silver | black; only used by bodies with colorways
  card: null,        // rendered canvas
};

// ---------- EXIF (partial-tolerant) ----------
async function readParams(file) {
  let tags = null;
  try {
    tags = await exifr.parse(file, {
      pick: ['Model', 'FocalLength', 'FNumber', 'ExposureTime', 'ISO', 'LensModel'],
    });
  } catch { /* unreadable EXIF -> all fields empty */ }
  return {
    model: tags && tags.Model ? String(tags.Model).trim() : '',
    lens: tags && tags.LensModel ? String(tags.LensModel).trim() : '',
    focal: tags && isFinite(Number(tags.FocalLength)) ? Number(tags.FocalLength) : NaN,
    fnum: tags && isFinite(Number(tags.FNumber)) ? Number(tags.FNumber) : NaN,
    expo: tags && isFinite(Number(tags.ExposureTime)) ? Number(tags.ExposureTime) : NaN,
    iso: tags && Number(tags.ISO) > 0 ? Number(tags.ISO) : NaN,
  };
}

function fmtShutter(sec) {
  if (sec >= 1) return `${trimNum(sec)} s`;
  return `1/${Math.round(1 / sec)} s`;
}
function trimNum(n) {
  return String(parseFloat(Number(n).toPrecision(6)));
}

// ---------- geometry helpers ----------
function canvasSize(pw, ph) {
  if (pw >= ph) return { w: CANVAS_LONG, h: Math.max(1, Math.round(CANVAS_LONG * ph / pw)) };
  return { w: Math.max(1, Math.round(CANVAS_LONG * pw / ph)), h: CANVAS_LONG };
}

// draw img cover-cropped into rect (never stretch)
function drawCover(ctx, img, x, y, w, h) {
  const s = Math.max(w / img.width, h / img.height);
  const dw = img.width * s, dh = img.height * s;
  ctx.drawImage(img, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh);
}

// blur fallback for browsers without ctx.filter: shrink hard, then stretch
// back with high-quality smoothing (two passes approximate a gaussian)
function drawCoverBlurFallback(ctx, img, w, h) {
  const k = 12;
  const sw = Math.max(8, Math.round(w / k)), sh = Math.max(8, Math.round(h / k));
  const off = document.createElement('canvas');
  off.width = sw; off.height = sh;
  const octx = off.getContext('2d');
  octx.imageSmoothingEnabled = true;
  octx.imageSmoothingQuality = 'high';
  const s0 = Math.max(sw / img.width, sh / img.height);
  octx.drawImage(img, (sw - img.width * s0) / 2, (sh - img.height * s0) / 2,
                 img.width * s0, img.height * s0);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(off, 0, 0, w + 40, h + 40);
  ctx.drawImage(off, -8, -8, w + 16, h + 16);
}

// ---------- pipeline ----------
async function loadBody(profileUrl, colorway) {
  const prof = await (await fetch(profileUrl)).json();
  const base = new URL(profileUrl, location.href);
  let pngName = prof.png;
  let quad = prof.screen_quad;
  const cw = colorway && colorway !== 'silver' &&
             prof.colorways && prof.colorways[colorway];
  if (cw) {
    pngName = cw.png;
    if (cw.screen_quad) quad = cw.screen_quad;  // per-colorway geometry
  }
  const png = await new Promise((ok, no) => {
    const im = new Image();
    im.onload = () => ok(im);
    im.onerror = () => no(new Error('机身图加载失败'));
    im.src = new URL(pngName, base).href;
  });
  return { prof, png, quad };
}

// low-contrast gray look for placeholder bodies (e.g. D850)
function grayCanvas(img) {
  const c = document.createElement('canvas');
  c.width = img.width; c.height = img.height;
  const g = c.getContext('2d');
  g.drawImage(img, 0, 0);
  if (FILTER_OK) {
    g.filter = 'grayscale(1) contrast(0.72) brightness(0.96)';
    g.clearRect(0, 0, c.width, c.height);
    g.drawImage(img, 0, 0);
    g.filter = 'none';
  } else {
    // legacy fallback: manual desaturate + contrast around mid-gray
    const d = g.getImageData(0, 0, c.width, c.height);
    const px = d.data;
    for (let i = 0; i < px.length; i += 4) {
      if (!px[i + 3]) continue;
      const v = 0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2];
      const o = ((v - 128) * 0.72 + 128) * 0.96;
      px[i] = px[i + 1] = px[i + 2] = o;
    }
    g.putImageData(d, 0, 0);
  }
  return c;
}

function renderFromState() {
  if (!state.img) return Promise.resolve();
  return render();
}

async function render() {
  const { img, params } = state;
  const b = BODIES.find(b => b.key === state.bodyKey);
  const body = await loadBody(b.profile,
                              b.hasColorways ? state.colorway : null);

  const { w: cw, h: ch } = canvasSize(img.naturalWidth, img.naturalHeight);
  const card = document.createElement('canvas');
  card.width = cw; card.height = ch;
  const ctx = card.getContext('2d');

  // 1. background: cover + blur + darken
  if (FILTER_OK) {
    ctx.filter = `blur(${cw * BG_BLUR_FRAC}px)`;
    drawCoverOverscan(ctx, img, cw, ch, Math.ceil(cw * BG_BLUR_FRAC * 3));
    ctx.filter = 'none';
  } else {
    drawCoverBlurFallback(ctx, img, cw, ch);
  }
  ctx.fillStyle = `rgba(0,0,0,${1 - BG_DARKEN})`;
  ctx.fillRect(0, 0, cw, ch);

  // 2+3. layer sandwich: photo (middle) UNDER the body (top). The body
  //      canvas gets a transparent window punched at the screen quad, so
  //      the bezel masks any paste overflow; the paste rect is the quad
  //      expanded a few px, clamped to the body's opaque silhouette.
  const bodyImg = body.prof.gray ? grayCanvas(body.png) : body.png;
  let quad = body.quad.map(p => p.slice());
  const bw0 = bodyImg.width, bh0 = bodyImg.height;
  const portrait = ch >= cw;

  // opaque bbox of the body (for clamping the paste expansion)
  const bb = document.createElement('canvas');
  bb.width = bw0; bb.height = bh0;
  const bbg = bb.getContext('2d');
  bbg.drawImage(bodyImg, 0, 0);
  const bd = bbg.getImageData(0, 0, bw0, bh0).data;
  let bx0 = bw0, by0 = bh0, bx1 = 0, by1 = 0;
  for (let y = 0; y < bh0; y++) {
    for (let x = 0; x < bw0; x++) {
      if (bd[(y * bw0 + x) * 4 + 3] > 10) {
        if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
        if (y < by0) by0 = y; if (y > by1) by1 = y;
      }
    }
  }
  // punch the window: quad inset 2px, rounded like the real glass
  const xs0 = quad.map(p => p[0]), ys0 = quad.map(p => p[1]);
  const ql = Math.min(...xs0), qt = Math.min(...ys0);
  const qr = Math.max(...xs0), qb = Math.max(...ys0);
  const inset = 2, rad = Math.max(4, Math.round((qr - ql) * 0.03));
  bbg.globalCompositeOperation = 'destination-out';
  bbg.beginPath();
  bbg.roundRect(ql + inset, qt + inset, qr - ql - 2 * inset,
                qb - qt - 2 * inset, rad);
  bbg.fill();
  bbg.globalCompositeOperation = 'source-over';
  // expanded paste quad, clamped to the silhouette
  const exp = 6;
  const el = Math.min(exp, ql - bx0), et = Math.min(exp, qt - by0);
  const er = Math.min(exp, bx1 - qr), eb = Math.min(exp, by1 - qb);
  let pquad = [[ql - el, qt - et], [qr + er, qt - et],
               [qr + er, qb + eb], [ql - el, qb + eb]];

  const drawn = document.createElement('canvas');
  const dctx = drawn.getContext('2d');
  if (portrait) {
    drawn.width = bh0; drawn.height = bw0;
    dctx.translate(0, bw0);
    dctx.rotate(-Math.PI / 2);
    dctx.drawImage(bb, 0, 0);
    pquad = pquad.map(([x, y]) => [y, bw0 - x]);
  } else {
    drawn.width = bw0; drawn.height = bh0;
    dctx.drawImage(bb, 0, 0);
  }
  const topMargin = Math.round(ch * BODY_TOP_FRAC);
  const textZone = 56 + GAP_LINE + LINE2_SIZE + 30;
  const sBody = Math.min(cw * BODY_WIDTH_FRAC / drawn.width,
                         (ch - topMargin - textZone) / drawn.height);
  const bw = Math.round(drawn.width * sBody);
  const bh = Math.round(drawn.height * sBody);
  const bx = Math.round((cw - bw) / 2);
  // always centered: horizontally on the canvas, vertically between the
  // top margin and the caption zone
  const by = topMargin + Math.round((ch - topMargin - textZone - bh) / 2);

  // middle layer: screen paste (cover-crop, square corners — the body's
  // bezel masks them), scale vs pre-resize width; per-body zoom falls out
  // of the quad size automatically
  const s = bw / drawn.width;
  const xs = pquad.map(p => p[0]), ys = pquad.map(p => p[1]);
  const left = Math.min(...xs) * s + bx, top = Math.min(...ys) * s + by;
  const qw = Math.round((Math.max(...xs) - Math.min(...xs)) * s);
  const qh = Math.round((Math.max(...ys) - Math.min(...ys)) * s);
  ctx.save();
  ctx.beginPath();
  ctx.rect(left, top, qw, qh);
  ctx.clip();
  if (FILTER_OK) {
    ctx.filter = `brightness(${SCREEN_BRIGHTEN})`;
    drawCover(ctx, img, left, top, qw, qh);
  } else {
    drawCover(ctx, img, left, top, qw, qh);
    ctx.fillStyle = `rgba(255,255,255,${SCREEN_BRIGHTEN - 1})`;
    ctx.fillRect(left, top, qw, qh);
  }
  ctx.restore();
  // top layer: body with the punched window
  ctx.drawImage(drawn, bx, by, bw, bh);

  // 4. caption lines, pinned to the card bottom, auto-shrink on overflow;
  const parts1 = [];
  if (params.model) parts1.push(params.model);
  if (params.lens) parts1.push(params.lens);
  const bSel = BODIES.find(x => x.key === state.bodyKey);
  const line1 = parts1.join(' · ') ||
                `${bSel.brand ? bSel.brand + ' ' : ''}${bSel.label}`;
  const parts2 = [];
  if (isFinite(params.focal)) parts2.push(`${trimNum(params.focal)} mm`);
  if (isFinite(params.expo)) parts2.push(fmtShutter(params.expo));
  if (isFinite(params.fnum)) parts2.push(`f/${trimNum(params.fnum)}`);
  if (isFinite(params.iso)) parts2.push(`ISO ${Math.round(params.iso)}`);
  const lines = [line1, parts2.join(' · ')].filter(Boolean);
  ctx.textBaseline = 'alphabetic';
  let y = ch - 24 - LINE2_SIZE - GAP_LINE * (lines.length - 1);  // bottom-pinned
  const maxTextW = cw - 48;
  for (const [text, size, font] of lines.map((t, i) =>
      [t, i === lines.length - 1 ? LINE2_SIZE : LINE1_SIZE,
       i === lines.length - 1 ? FONT_MONO : FONT_SANS])) {
    let sizeFit = size;
    ctx.font = `${sizeFit}px ${font}`;
    while (ctx.measureText(text).width > maxTextW && sizeFit > 14) {
      sizeFit -= 1;
      ctx.font = `${sizeFit}px ${font}`;
    }
    const tw = ctx.measureText(text).width;
    ctx.fillStyle = 'rgba(255,255,255,0.92)';
    ctx.fillText(text, (cw - tw) / 2, y + sizeFit * 0.8);
    y += GAP_LINE;
  }

  state.card = card;
  els.preview.src = card.toDataURL('image/jpeg', 0.93);
}

// cover variant with overscan so the blur filter never reveals raw edges
function drawCoverOverscan(ctx, img, w, h, margin) {
  const x = -margin, y = -margin, bw = w + margin * 2, bh = h + margin * 2;
  const s = Math.max(bw / img.width, bh / img.height);
  const dw = img.width * s, dh = img.height * s;
  ctx.drawImage(img, x + (bw - dw) / 2, y + (bh - dh) / 2, dw, dh);
}

// ---------- errors ----------
function showError(msg) {
  els.err.textContent = msg;
  els.err.style.display = 'block';
}
function clearError() {
  els.err.style.display = 'none';
}

// ---------- flow ----------
async function decodeImage(file) {
  const url = URL.createObjectURL(file);
  try {
    const img = await new Promise((ok, no) => {
      const im = new Image();
      im.onload = () => ok(im);
      im.onerror = () => no(new Error('这张照片浏览器解不开，请用 JPG 再试'));
      im.src = url;
    });
    return img;
  } finally {
    setTimeout(() => URL.revokeObjectURL(url), 10000);
  }
}

async function handleFile(file) {
  if (!file) return;
  clearError();
  try {
    state.file = file;
    state.img = await decodeImage(file);
    const p = await readParams(file);
    state.params = p;

    const notices = [];
    if (!p.model) notices.push('未解析到机型信息，卡片将显示当前选中的机身');
    if (!isFinite(p.focal) && !isFinite(p.expo) &&
        !isFinite(p.fnum) && !isFinite(p.iso)) {
      notices.push('未解析到拍摄参数，参数行将留空（常见于聊天软件传输的照片）');
    } else {
      const miss = [];
      if (!isFinite(p.focal)) miss.push('焦距');
      if (!isFinite(p.expo)) miss.push('快门');
      if (!isFinite(p.fnum)) miss.push('光圈');
      if (!isFinite(p.iso)) miss.push('ISO');
      if (miss.length) notices.push(`未解析到：${miss.join('、')}（已留空）`);
    }
    if (notices.length) showError(notices.join('；'));

    await refreshProfileAliases();
    if (p.model) {
      const matched = BODIES.find(b => b.ready && (b.aliases || []).includes(p.model));
      if (matched) {
        state.bodyKey = matched.key;
        state.colorway = 'silver';
        syncSel();
      }
    }
    await render();
    els.drop.style.display = 'none';
    els.previewWrap.style.display = 'block';
    els.controls.style.display = 'flex';
  } catch (e) {
    showError(e.message || '处理失败');
  }
}

const aliasCache = {};
async function refreshProfileAliases() {
  for (const b of BODIES.filter(b => b.ready && aliasCache[b.key] === undefined)) {
    try {
      const p = await (await fetch(b.profile)).json();
      aliasCache[b.key] = p.aliases || [];
    } catch { aliasCache[b.key] = []; }
    b.aliases = aliasCache[b.key];
  }
}

// ---------- body switcher (dropdown) + colorway toggle ----------
function buildSel() {
  els.bodySel.innerHTML = '';
  for (const b of BODIES) {
    const opt = document.createElement('option');
    opt.value = b.key;
    opt.textContent = b.ready ? b.label : `${b.label}（待支持）`;
    if (!b.ready) opt.disabled = true;
    els.bodySel.appendChild(opt);
  }
  syncSel();
}
function syncSel() {
  els.bodySel.value = state.bodyKey;
  const b = BODIES.find(x => x.key === state.bodyKey);
  els.cwSeg.hidden = !b || !b.hasColorways;
  for (const btn of els.cwSeg.querySelectorAll('button')) {
    btn.classList.toggle('on', btn.dataset.cw === state.colorway);
  }
}
els.bodySel.addEventListener('change', () => {
  if (state.bodyKey === els.bodySel.value) return;
  state.bodyKey = els.bodySel.value;
  state.colorway = 'silver';  // reset colorway on body switch
  syncSel();
  if (state.img) renderFromState().catch(showError);
});
els.cwSeg.addEventListener('click', e => {
  const btn = e.target.closest('button[data-cw]');
  if (!btn || state.colorway === btn.dataset.cw) return;
  state.colorway = btn.dataset.cw;
  syncSel();
  if (state.img) renderFromState().catch(showError);
});

// ---------- wire up ----------
els.drop.addEventListener('click', () => els.file.click());
els.repick.addEventListener('click', () => els.file.click());
els.file.addEventListener('change', () => handleFile(els.file.files[0]));
['dragover', 'dragenter'].forEach(ev =>
  els.drop.addEventListener(ev, e => { e.preventDefault(); els.drop.classList.add('over'); }));
['dragleave', 'drop'].forEach(ev =>
  els.drop.addEventListener(ev, e => { e.preventDefault(); els.drop.classList.remove('over'); }));
els.drop.addEventListener('drop', e => handleFile(e.dataTransfer.files[0]));
window.addEventListener('paste', e => {
  const item = [...(e.clipboardData?.files || [])].find(f => f.type.startsWith('image/'));
  if (item) handleFile(item);
});

els.save.addEventListener('click', () => {
  if (!state.card) return;
  state.card.toBlob(blob => {
    const stem = (state.file?.name || 'card').replace(/\.[^.]+$/, '');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${stem}_wm.jpg`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }, 'image/jpeg', 0.93);
});

buildSel();
