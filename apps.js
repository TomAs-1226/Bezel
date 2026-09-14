// Apps open in a window that grows out of the icon you touched and shrinks back into it. Every part of that is
// interruptible, as Apple's fluid interfaces require: drag an app down and it becomes a card under your finger;
// let go and it flies home or back open with your velocity; catch a closing app and it stops where you touched;
// tap it, or its icon, and it reopens from wherever it was.

import { Motion, SPRINGS, project, rubberBand, velocityTracker } from './motion.js';
import { hex, mixRgb } from './panel.js';
import { createRaster } from './raster.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

export function createApps(ctx) {
  const { screen, settings } = ctx;
  const layer = screen.querySelector('#app-layer');
  const apps = new Map([...layer.querySelectorAll('.app')].map((el) => [el.dataset.app, { el, raster: null, module: null }]));
  const grow = new Motion(0, { spring: SPRINGS.release, epsilon: 0.0005 });
  const offsetX = new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 });
  const offsetY = new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 });
  const lift = new Motion(0, { spring: SPRINGS.release, epsilon: 0.002 }); // how card-like the window is
  const tracker = velocityTracker();
  const pending = new Set();
  let current = null;
  let from = null;
  let tile = null;
  let drag = null;
  let caught = false;

  const rectOf = (el) => {
    const s = screen.getBoundingClientRect();
    const r = el.getBoundingClientRect();
    return { x: r.left - s.left, y: r.top - s.top, w: r.width, h: r.height, r: parseFloat(getComputedStyle(el).borderTopLeftRadius) || 0 };
  };
  const closing = () => current && grow.target === 0;
  const full = () => current && !drag && grow.target === 1 && grow.value > 0.999 && !grow.moving && Math.abs(offsetY.value) < 0.5;

  // Hidden apps measure as nothing, so an app is measured and painted as it opens, and again after its rows change.
  function paint(app) {
    const saved = layer.style.transform;
    layer.style.transform = 'none';
    ctx.measureWithin(app.el);
    app.raster ??= createRaster(app.el);
    app.raster.draw();
    ctx.upload('app', app.raster.canvas, null);
    layer.style.transform = saved;
  }
  ctx.appRepaint = () => { if (current && !closing()) paint(apps.get(current)); };

  function finish() {
    if (!current) return;
    apps.get(current).el.hidden = true;
    layer.hidden = true;
    current = null;
    tile = null;
    offsetX.set(0);
    offsetY.set(0);
    lift.set(0);
  }

  function open(name, iconTile) {
    const app = apps.get(name);
    if (!app) return;
    // The same app, still on its way home: turn it round from wherever it is, with the speed it has.
    if (current === name) {
      grow.to(1, { spring: SPRINGS.release });
      offsetX.to(0);
      offsetY.to(0);
      lift.to(0);
      ctx.setAppOpen(true);
      app.module?.open?.();
      return;
    }
    if (current) finish();
    for (const [key, other] of apps) other.el.hidden = key !== name;
    current = name;
    tile = iconTile;
    from = { ...rectOf(iconTile), color: hex(getComputedStyle(iconTile).getPropertyValue('--tile') || '#333') };
    layer.hidden = false;
    app.module?.beforeOpen?.();
    paint(app);
    app.module?.open?.();
    ctx.setAppOpen(true);
    grow.set(0);
    grow.to(1, { spring: SPRINGS.release });
  }

  function close(velocity = 0) {
    if (!current || closing()) return;
    apps.get(current).module?.close?.();
    ctx.setAppOpen(false);
    grow.to(0, { spring: SPRINGS.release, velocity });
    offsetX.to(0, { spring: SPRINGS.release });
    offsetY.to(0, { spring: SPRINGS.release });
  }

  // Drag an app down, anywhere its content doesn't claim the drag, and it becomes a card under your finger.
  const dismiss = {
    begin() {
      drag = { k0: grow.value, y0: offsetY.value };
      grow.set(grow.value);
      offsetY.set(offsetY.value);
      lift.to(1, { spring: SPRINGS.hold });
      tracker.reset();
      ctx.setAppOpen(true);
    },
    move(dy, t) {
      const H = ctx.H;
      const down = Math.max(0, drag.y0 + dy);
      const up = Math.min(0, drag.y0 + dy);
      const k = clamp(drag.k0 - down / (H * 0.9), 0.3, 1);
      tracker.add(t, down);
      grow.set(k, -tracker.velocity(t) / (H * 0.9));
      offsetY.set(down * 0.45 - rubberBand(-up, H) * 0.3);
    },
    end(t) {
      const v = tracker.velocity(t);
      const H = ctx.H;
      drag = null;
      lift.to(0, { spring: SPRINGS.release });
      // project where the card is heading: past a quarter of the way home, it goes home
      const aim = grow.value - project(v, 0.994) / (H * 0.9);
      if (aim < 0.78) close(-v / (H * 0.9));
      else { grow.to(1, { spring: SPRINGS.release, velocity: -v / (H * 0.9) }); offsetY.to(0); ctx.setAppOpen(true); }
    },
  };
  for (const { el } of apps.values()) ctx.addDragTarget(el, dismiss, 'y');

  // Catch a closing app: it stops under your finger. Let go without dragging and it opens again.
  layer.addEventListener('pointerdown', () => {
    if (!closing() || grow.value < 0.04) return;
    caught = true;
    grow.set(grow.value);
    offsetY.set(offsetY.value);
  }, true);
  window.addEventListener('pointerup', () => {
    if (!caught) return;
    caught = false;
    if (!drag) open(current, tile);
  });

  // Text inside an app repaints only while its window is fully open; changes made mid-flight wait for that.
  ctx.setExtraDirty((el) => {
    const app = current && apps.get(current);
    if (!app?.raster || !app.el.contains(el)) return;
    if (!full()) { pending.add(el); return; }
    const region = app.raster.update(el);
    if (region) ctx.upload('app', app.raster.canvas, region);
  });

  function geometry() {
    const k = grow.value;
    const t = clamp(k, 0, 1);
    const W = ctx.W;
    const H = ctx.H;
    const w = from.w + (W - from.w) * k;
    const h = from.h + (H - from.h) * k;
    const x = from.x * (1 - k) + offsetX.value;
    const y = from.y * (1 - k) + offsetY.value;
    const card = clamp(lift.value + (1 - t) * 3, 0, 1);
    const r = from.r + (2 * ctx.unit - from.r) * t + card * t * 34 * ctx.unit;
    return { x, y, w, h, r, t };
  }

  ctx.setAppWindow(() => {
    if (!current) return null;
    const g = geometry();
    return {
      x: g.x, y: g.y, w: g.w, h: g.h, r: g.r,
      color: mixRgb(from.color, settings.rgb.ground, clamp(g.t * 1.6, 0, 1)),
      shadow: settings.calm ? 0 : 0.3 * Math.min(1, Math.sin(Math.PI * g.t) + lift.value * 0.6),
      full: full(),
      glass: clamp((g.t - 0.7) / 0.3, 0, 1),
      el: apps.get(current).el,
    };
  });

  layer.addEventListener('click', (e) => { if (e.target.closest('.app-back')) close(); });
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape' && current) close(); });

  ctx.onMeasure({
    before() { layer.style.transform = 'none'; },
    after() { if (current) paint(apps.get(current)); },
  });
  ctx.onRetone(() => { if (current) paint(apps.get(current)); });

  // The launcher reads these to keep the icon the window will land in hidden until it lands.
  Object.defineProperties(ctx, {
    appProgress: { get: () => (current ? clamp(grow.value, 0, 1) : 0), configurable: true },
    appTile: { get: () => tile, configurable: true },
  });

  ctx.onFrame((t, dt) => {
    if (!current) return {};
    if (closing() && !drag && !caught && grow.value < 0.002 && !grow.moving) { finish(); return {}; }
    const g = geometry();
    const W = ctx.W;
    const H = ctx.H;
    const sx = g.w / W;
    const sy = g.h / H;
    layer.style.transform = `translate(${g.x.toFixed(2)}px, ${g.y.toFixed(2)}px) scale(${sx.toFixed(5)}, ${sy.toFixed(5)})`;
    const app = apps.get(current);
    if (full() && pending.size) {
      for (const el of pending) {
        const region = app.raster.update(el);
        if (region) ctx.upload('app', app.raster.canvas, region);
      }
      pending.clear();
    }
    const extra = app.module?.frame?.(t, dt, { k: grow.value, full: full(), win: { x: g.x, y: g.y, sx, sy } }) || {};
    return { ...extra, appDraw: app.raster ? { name: 'app', x: 0, y: 0, w: app.raster.width, h: app.raster.height } : null };
  });

  return {
    open, close,
    get current() { return current; },
    /** Gives an app its behaviour: { beforeOpen, open, close, frame(t, dt, { k, full, win }) → extra render state }. */
    register(name, module) { const app = apps.get(name); if (app) app.module = module; },
  };
}
