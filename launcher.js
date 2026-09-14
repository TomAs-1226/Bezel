// The app launcher. Arriving on it, the icons cascade: each springs into place on its own Detent spring,
// delayed by its distance from where you came from, so the grid ripples out from the dock's apps button or
// sweeps in from the edge you swiped from. Leaving folds them away again so the next visit cascades too.
// Icons are live DOM (never under glass): the cascade moves each one separately, which a rasterized layer can't.

import { Motion, SPRINGS } from './motion.js';
import { APPS } from './panel.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const SPREAD = 0.28; // seconds from the nearest icon to the farthest

export function createLauncher(ctx, { open }) {
  const { screen, settings } = ctx;
  const view = screen.querySelector('#apps-view');
  const dockApps = screen.querySelector('.dock-item[data-section="1"]');
  const icons = [...view.querySelectorAll('.app-icon')].map((el) => ({
    el, tile: el.querySelector('.app-tile'),
    k: new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 }),
    press: new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 }),
    cx: 0, cy: 0, start: Infinity,
  }));
  let origin = { x: 360, y: 700 };
  let folded = true;
  // While an app is open its window covers the grid, and live icons would draw over it, so they step back.
  const veil = new Motion(0, { spring: SPRINGS.layer, epsilon: 0.002 });

  function cascade(fromX, fromY) {
    const now = performance.now() / 1000;
    const far = Math.max(1, ...icons.map((i) => Math.hypot(i.cx - fromX, i.cy - fromY)));
    origin = { x: fromX, y: fromY };
    folded = false;
    for (const i of icons) {
      i.k.set(0);
      i.start = now + (settings.calm ? 0 : (Math.hypot(i.cx - fromX, i.cy - fromY) / far) * SPREAD);
    }
  }

  ctx.onIndex((index, from) => {
    if (index !== APPS || from === APPS) return;
    const W = ctx.W;
    if (Math.abs(from - APPS) === 1) {
      // swiped in: the leading edge is where the page came from
      cascade(from < APPS ? W : 0, ctx.H * 0.45);
    } else {
      const b = dockApps.getBoundingClientRect();
      const s = screen.getBoundingClientRect();
      cascade(b.left + b.width / 2 - s.left, b.top + b.height / 2 - s.top);
    }
  });

  ctx.onMeasure({
    before() { for (const i of icons) i.el.style.transform = 'none'; },
    after(rel) {
      for (const i of icons) {
        const r = rel(i.tile);
        i.cx = r.x - APPS * ctx.W + r.w / 2;
        i.cy = r.y + r.h / 2;
      }
    },
  });

  for (const i of icons) {
    i.el.addEventListener('pointerdown', (e) => { if (e.button <= 0) i.press.to(1, { spring: SPRINGS.hold }); });
    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) i.el.addEventListener(type, () => i.press.to(0, { spring: SPRINGS.release }));
    i.el.addEventListener('click', () => {
      const go = i.el.dataset.go;
      if (go !== undefined) ctx.go(Number(go));
      else open(i.el.dataset.app, i.tile);
    });
  }

  ctx.onFrame((t) => {
    // Once the launcher has left the screen, fold the icons so the next arrival cascades again.
    const away = Math.abs(ctx.page + APPS * ctx.W) >= ctx.W;
    if (away && ctx.index !== APPS && !folded) {
      folded = true;
      for (const i of icons) { i.k.set(0); i.start = Infinity; }
    }
    // Icons step back as an app grows over them and return as it shrinks; the icon it lands in stays hidden until then.
    const cover = ctx.appProgress ?? 0;
    const u = ctx.unit;
    // live icons draw above the glass, so the ones under the floating activity stack step back instead
    const stack = ctx.activitiesRect;
    const shiftX = ctx.page + APPS * ctx.W;
    for (const i of icons) {
      if (t >= i.start) { i.start = Infinity; i.k.to(1, { spring: SPRINGS.release }); }
      const k = i.k.value;
      const dx = i.cx - origin.x;
      const dy = i.cy - origin.y;
      const d = Math.hypot(dx, dy) || 1;
      const travel = settings.calm ? 0 : (1 - k) * 46 * u;
      const scale = (settings.calm ? 1 : 0.72 + 0.28 * k) * (1 - 0.07 * i.press.value);
      i.el.style.transform = `translate(${((-dx / d) * travel).toFixed(2)}px, ${((-dy / d) * travel).toFixed(2)}px) scale(${scale.toFixed(4)})`;
      const home = i.tile === ctx.appTile ? (cover > 0.015 ? 0 : 1) : 1 - clamp(cover * 2.5, 0, 1);
      const under = stack && i.cx + shiftX > stack.x - 40 * u && i.cx + shiftX < stack.x + stack.w + 40 * u && i.cy > stack.y - 40 * u && i.cy < stack.y + stack.h + 40 * u;
      i.el.style.opacity = (clamp(k * 1.6, 0, 1) * home * (under ? 0.14 : 1)).toFixed(3);
    }
  });

  return { icons };
}
