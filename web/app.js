/* app.js — see-through-camera, browser port of cli/watermark.py.
 *
 * Pipeline parity with the Python version:
 *   EXIF -> orientation normalize -> cover+blur+darken bg -> body layer
 *   (portrait: rotate visual-CCW, grip up; quad remap) -> screen paste
 *   (cover-crop, square corners) -> two centered text lines -> JPEG.
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

// calibrated bodies only; others render as disabled choices
const BODIES = [
  { key: 'z63', label: 'Z 6III', profile: 'bodies/z63/profile.json', ready: true },
  { key: 'zr',  label: 'ZR',     profile: 'bodies/zr/profile.json',  ready: true },
  { key: 'z5',  label: 'Z 5',    profile: 'bodies/z5/profile.json',  ready: true },
  { key: 'z8',  label: 'Z 8',    profile: 'bodies/z8/profile.json',  ready: true },
  { key: 'z9',  label: 'Z 9',    profile: 'bodies/z9/profile.json',  ready: true },
  { key: 'zf',  label: 'Z f',    profile: 'bodies/zf/profile.json',  ready: true },
  { key: 'zfc', label: 'Z fc',   profile: 'bodies/zfc/profile.json', ready: true },
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
  bodySeg: $('bodySeg'), repick: $('repick'), save: $('save'),
};
let state = {
  file: null,        // picked File
  img: null,         // HTMLImageElement, orientation-normalized by the browser
  params: null,      // partial: any of model/lens/focal/expo/fnum/iso
  bodyKey: 'z63',    // user-selected body (default z63)
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
async function loadBody(profileUrl) {
  const prof = await (await fetch(profileUrl)).json();
  const base = new URL(profileUrl, location.href);
  const png = await new Promise((ok, no) => {
    const im = new Image();
    im.onload = () => ok(im);
    im.onerror = () => no(new Error('机身图加载失败'));
    im.src = new URL(prof.png, base).href;
  });
  return { prof, png };
}

async function render() {
  const { img, params } = state;
  const body = await loadBody(BODIES.find(b => b.key === state.bodyKey).profile);

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

  // 2. body layer; portrait: rotate visual-CCW (grip up), remap quad
  let quad = body.prof.screen_quad.map(p => p.slice());
  const bw0 = body.png.width, bh0 = body.png.height;
  const portrait = ch >= cw;
  const drawn = document.createElement('canvas');
  const dctx = drawn.getContext('2d');
  if (portrait) {
    drawn.width = bh0; drawn.height = bw0;
    dctx.translate(0, bw0);
    dctx.rotate(-Math.PI / 2);
    dctx.drawImage(body.png, 0, 0);
    quad = quad.map(([x, y]) => [y, bw0 - x]);
  } else {
    drawn.width = bw0; drawn.height = bh0;
    dctx.drawImage(body.png, 0, 0);
  }
  const topMargin = Math.round(ch * BODY_TOP_FRAC);
  const textZone = 56 + GAP_LINE + LINE2_SIZE + 30;
  const sBody = Math.min(cw * BODY_WIDTH_FRAC / drawn.width,
                         (ch - topMargin - textZone) / drawn.height);
  const bw = Math.round(drawn.width * sBody);
  const bh = Math.round(drawn.height * sBody);
  const bx = Math.round((cw - bw) / 2), by = topMargin;
  ctx.drawImage(drawn, bx, by, bw, bh);   // body first: its LCD is opaque

  // 3. screen paste (cover-crop, square corners), scale vs pre-resize width
  const s = bw / drawn.width;
  const xs = quad.map(p => p[0]), ys = quad.map(p => p[1]);
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

  // 4. caption lines, pinned to the card bottom, auto-shrink on overflow;
  //    unparsed fields are simply left out
  const bodyLabel = BODIES.find(b => b.key === state.bodyKey).label;
  const parts1 = [];
  if (params.model) parts1.push(params.model);
  if (params.lens) parts1.push(params.lens);
  const line1 = parts1.join(' · ') || `NIKON ${bodyLabel}`;
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
        syncSeg();
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

// ---------- body switcher ----------
function buildSeg() {
  els.bodySeg.innerHTML = '';
  for (const b of BODIES) {
    const btn = document.createElement('button');
    btn.textContent = b.label;
    btn.dataset.key = b.key;
    if (!b.ready) {
      btn.disabled = true;
      if (b.tip) btn.dataset.tip = b.tip;
    }
    btn.addEventListener('click', () => {
      if (state.bodyKey === b.key) return;
      state.bodyKey = b.key;
      syncSeg();
      renderFromState().catch(showError);
    });
    els.bodySeg.appendChild(btn);
  }
  syncSeg();
}
function syncSeg() {
  for (const btn of els.bodySeg.children) {
    btn.classList.toggle('on', btn.dataset.key === state.bodyKey);
  }
}

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

buildSeg();
