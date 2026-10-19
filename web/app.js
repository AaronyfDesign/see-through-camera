/* app.js — see-through-camera, browser port of cli/watermark.py.
 *
 * Pipeline parity with the Python version:
 *   EXIF -> orientation normalize -> cover+blur+darken bg -> body layer
 *   (portrait: rotate visual-CCW, grip up; quad remap) -> screen paste
 *   (cover-crop, square corners) -> two centered text lines -> JPEG.
 *
 * Hard-error discipline (S10/S11): a missing EXIF field is an error, never
 * silently defaulted.
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
  { key: 'zr',  label: 'ZR',     profile: null, ready: false, tip: '标定中' },
  { key: 'z5',  label: 'Z 5',    profile: null, ready: false, tip: '标定中' },
  { key: 'z8',  label: 'Z 8',    profile: null, ready: false, tip: '标定中' },
  { key: 'z9',  label: 'Z 9',    profile: null, ready: false, tip: '标定中' },
  { key: 'zf',  label: 'Z f',    profile: null, ready: false, tip: '标定中' },
  { key: 'zfc', label: 'Z fc',   profile: null, ready: false, tip: '标定中' },
];

// ---------- state ----------
const $ = (id) => document.getElementById(id);
const els = {
  drop: $('drop'), file: $('file'), previewWrap: $('previewWrap'),
  preview: $('preview'), err: $('err'), controls: $('controls'),
  bodySeg: $('bodySeg'), repick: $('repick'), save: $('save'),
};
let state = {
  file: null,        // File object
  bitmap: null,      // ImageBitmap, already orientation-normalized
  exif: null,        // parsed EXIF subset
  bodyKey: 'z63',    // user-selected body (default z63)
  card: null,        // rendered canvas
};

// ---------- EXIF ----------
async function readExif(file) {
  const tags = await exifr.parse(file, {
    pick: ['Model', 'FocalLength', 'FNumber', 'ExposureTime', 'ISO', 'LensModel'],
  });
  const need = (v, name) => {
    if (v === undefined || v === null) {
      throw new Error(`这张照片缺少拍摄参数（${name}），无法生成卡片`);
    }
    return v;
  };
  return {
    model: String(need(tags.Model, '机型')).trim(),
    focal: need(tags.FocalLength, '焦距'),
    fnum: need(tags.FNumber, '光圈'),
    expo: need(tags.ExposureTime, '快门'),
    iso: need(tags.ISO, 'ISO'),
    lens: tags.LensModel ? String(tags.LensModel).trim() : '',
  };
}

function fmtShutter(sec) {
  if (sec >= 1) return `${trimNum(sec)} s`;
  return `1/${Math.round(1 / sec)} s`;
}
function trimNum(n) {
  // :g-equivalent — cut float noise without losing precision
  return String(parseFloat(n.toPrecision(6)));
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

// cover variant with overscan so the blur filter never reveals raw edges
// (Python blurs an already-cropped bitmap whose edge pixels extend; canvas
// 2D blur fades to transparent at the drawn image boundary instead)
function drawCoverOverscan(ctx, img, w, h, margin) {
  const x = -margin, y = -margin, bw = w + margin * 2, bh = h + margin * 2;
  const s = Math.max(bw / img.width, bh / img.height);
  const dw = img.width * s, dh = img.height * s;
  ctx.drawImage(img, x + (bw - dw) / 2, y + (bh - dh) / 2, dw, dh);
}

// ---------- pipeline ----------
async function loadBody(profileUrl) {
  const prof = await (await fetch(profileUrl)).json();
  const base = new URL(profileUrl, location.href); // profile.json absolute URL
  const png = await new Promise((ok, no) => {
    const im = new Image();
    im.onload = () => ok(im);
    im.onerror = () => no(new Error('机身图加载失败'));
    im.src = new URL(prof.png, base).href;
  });
  return { prof, png };
}

async function render() {
  const { bitmap, exif } = state;
  const body = await loadBody(BODIES.find(b => b.key === state.bodyKey).profile);

  const { w: cw, h: ch } = canvasSize(bitmap.width, bitmap.height);
  const card = document.createElement('canvas');
  card.width = cw; card.height = ch;
  const ctx = card.getContext('2d');

  // 1. background: cover + blur + darken
  ctx.filter = `blur(${cw * BG_BLUR_FRAC}px)`;
  drawCoverOverscan(ctx, bitmap, cw, ch, Math.ceil(cw * BG_BLUR_FRAC * 3));
  ctx.filter = 'none';
  ctx.fillStyle = `rgba(0,0,0,${1 - BG_DARKEN})`;
  ctx.fillRect(0, 0, cw, ch);

  // 2. body layer; portrait: rotate visual-CCW (grip up), remap quad
  let quad = body.prof.screen_quad.map(p => p.slice());
  let bw0 = body.png.width, bh0 = body.png.height;
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
  ctx.filter = `brightness(${SCREEN_BRIGHTEN})`;
  drawCover(ctx, bitmap, left, top, qw, qh);
  ctx.restore();

  // 4. text lines, centered below body, auto-shrink on overflow
  const lensPart = exif.lens ? ` · ${exif.lens}` : '';
  const line1 = `${exif.model}${lensPart}`;
  const line2 = `${trimNum(exif.focal)} mm · ${fmtShutter(exif.expo)} · ` +
                `f/${trimNum(exif.fnum)} · ISO ${exif.iso}`;
  ctx.textBaseline = 'alphabetic';
  let y = Math.min(by + bh + 56, ch - GAP_LINE - LINE2_SIZE - 24);
  const maxTextW = cw - 48;
  for (const [text, size, font] of [[line1, LINE1_SIZE, FONT_SANS],
                                    [line2, LINE2_SIZE, FONT_MONO]]) {
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

// ---------- errors ----------
function showError(msg) {
  els.err.textContent = msg;
  els.err.style.display = 'block';
}
function clearError() {
  els.err.style.display = 'none';
}

// ---------- flow ----------
async function handleFile(file) {
  if (!file) return;
  if (!/image\/jpeg/i.test(file.type)) {
    showError('目前支持 JPEG 照片');
    return;
  }
  clearError();
  try {
    const exif = await readExif(file);
    const bitmap = await createImageBitmap(file, { imageOrientation: 'from-image' });
    state.file = file;
    state.exif = exif;
    state.bitmap = bitmap;

    // auto-match body by EXIF model; fall back to current selection
    await refreshProfileAliases();
    const matched = BODIES.find(b => b.ready && (b.aliases || []).includes(exif.model));
    if (matched) {
      state.bodyKey = matched.key;
      syncSeg();
    } else {
      showError(`尚未收录这台机身（${exif.model}），先用当前选中机身生成`);
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
      render().catch(showError);
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
  const item = [...(e.clipboardData?.files || [])].find(f => f.type === 'image/jpeg');
  if (item) handleFile(item);
});

els.save.addEventListener('click', () => {
  if (!state.card) return;
  state.card.toBlob(blob => {
    const stem = (state.file?.name || 'card').replace(/\.jpe?g$/i, '');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${stem}_wm.jpg`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }, 'image/jpeg', 0.93);
});

buildSeg();
