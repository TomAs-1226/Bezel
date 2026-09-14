// Paints a DOM subtree's text, icons, images and marked shapes into a canvas, in the subtree's own
// coordinates. The WebGL scene draws that canvas under the glass, so the glass bends text like everything else.
// The DOM stays for layout, hit testing and accessibility; in WebGL mode its ink is made transparent.

// Glass draws its own labels, and live elements (launcher icons mid-cascade) are drawn by the DOM itself.
const SKIP = '[data-glass], [data-live]';

export function createRaster(root) {
  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  const images = new Map();
  let items = [];
  let width = 1;
  let height = 1;
  let dpr = 1;

  function collect(scope) {
    const box = root.getBoundingClientRect();
    const rel = (r) => ({ x: r.left - box.left, y: r.top - box.top, w: r.width, h: r.height });
    const out = [];
    const shapes = [...scope.querySelectorAll('[data-raster]')];
    if (scope.matches?.('[data-raster]')) shapes.unshift(scope);
    for (const el of shapes) {
      if (el.closest(SKIP)) continue;
      const style = getComputedStyle(el);
      if (style.visibility === 'hidden' || style.display === 'none') continue;
      out.push({
        kind: 'shape', el, ...rel(el.getBoundingClientRect()),
        image: el.dataset.raster === 'image' ? images.get(el) : null,
        fill: style.getPropertyValue('--raster-fill').trim() || style.backgroundColor, radius: parseFloat(style.borderTopLeftRadius) || 0,
      });
    }
    const walker = document.createTreeWalker(scope, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const el = node.parentElement;
      if (!el || !node.data.trim() || el.closest(SKIP)) continue;
      const style = getComputedStyle(el);
      if (style.display === 'none' || style.visibility === 'hidden') continue;
      const font = `${style.fontStyle} ${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
      const icon = el.classList.contains('icon');
      // One range per word keeps the DOM's own spacing and alignment even where canvas metrics differ.
      for (const m of node.data.matchAll(/\S+/g)) {
        const range = document.createRange();
        range.setStart(node, m.index);
        range.setEnd(node, m.index + m[0].length);
        const r = range.getBoundingClientRect();
        if (r.width < 0.5) continue;
        out.push({ kind: 'text', el, text: m[0], ...rel(r), font, color: style.color, spacing: style.letterSpacing, icon });
      }
    }
    return out;
  }

  const hit = (a, r) => a.x < r.x + r.w && a.x + a.w > r.x && a.y < r.y + r.h && a.y + a.h > r.y;

  function paint(region) {
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.save();
    if (region) {
      ctx.beginPath();
      ctx.rect(region.x, region.y, region.w, region.h);
      ctx.clip();
      ctx.clearRect(region.x, region.y, region.w, region.h);
    } else {
      ctx.clearRect(0, 0, width, height);
    }
    for (const it of items) {
      if (it.kind !== 'shape' || (region && !hit(it, region))) continue;
      ctx.beginPath();
      ctx.roundRect(it.x, it.y, it.w, it.h, Math.min(it.radius, it.w / 2, it.h / 2));
      if (it.image) {
        ctx.save();
        ctx.clip();
        ctx.drawImage(it.image, it.x, it.y, it.w, it.h);
        ctx.restore();
      } else {
        ctx.fillStyle = it.fill;
        ctx.fill();
      }
    }
    for (const it of items) {
      if (it.kind !== 'text' || (region && !hit(it, region))) continue;
      ctx.font = it.font;
      ctx.fillStyle = it.color;
      if ('letterSpacing' in ctx) ctx.letterSpacing = it.spacing === 'normal' ? '0px' : it.spacing;
      const m = ctx.measureText(it.text);
      const ascent = m.fontBoundingBoxAscent ?? it.h * 0.78;
      const descent = m.fontBoundingBoxDescent ?? it.h * 0.22;
      const baseline = it.y + (it.h - ascent - descent) / 2 + ascent;
      ctx.textAlign = it.icon ? 'center' : 'left';
      ctx.fillText(it.text, it.icon ? it.x + it.w / 2 : it.x, baseline);
    }
    ctx.restore();
  }

  return {
    canvas,
    get width() { return width; },
    get height() { return height; },
    setImage(el, image) { images.set(el, image); },

    /** Measures everything again and repaints the whole canvas. */
    draw() {
      width = Math.max(1, root.offsetWidth);
      height = Math.max(1, root.scrollHeight);
      items = collect(root);
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.ceil(width * dpr);
      const h = Math.ceil(height * dpr);
      if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
      paint(null);
    },

    /** Repaints one element after its text, colour or position changed. Returns the device-pixel region to upload. */
    update(el) {
      const before = items.filter((it) => el.contains(it.el));
      items = items.filter((it) => !el.contains(it.el)).concat(collect(el));
      const touched = before.concat(items.filter((it) => el.contains(it.el)));
      if (!touched.length) return null;
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (const it of touched) {
        x0 = Math.min(x0, it.x); y0 = Math.min(y0, it.y);
        x1 = Math.max(x1, it.x + it.w); y1 = Math.max(y1, it.y + it.h);
      }
      const r = { x: Math.max(0, x0 - 4), y: Math.max(0, y0 - 4) };
      r.w = Math.min(width, x1 + 4) - r.x;
      r.h = Math.min(height, y1 + 4) - r.y;
      if (r.w <= 0 || r.h <= 0) return null;
      paint(r);
      const X = Math.floor(r.x * dpr);
      const Y = Math.floor(r.y * dpr);
      return { x: X, y: Y, w: Math.min(canvas.width, Math.ceil((r.x + r.w) * dpr)) - X, h: Math.min(canvas.height, Math.ceil((r.y + r.h) * dpr)) - Y };
    },
  };
}
