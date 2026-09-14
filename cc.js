// Control center. Pull down from the top edge and glass modules cascade down under your finger, each a beat
// behind the one before; the page behind dims and steps back. It tracks the pull 1:1, is interruptible at any
// point, and settles open or closed with your velocity. Push it back up, or tap the dimmed page, to close it.

import { Motion, SPRINGS, project, rubberBand, velocityTracker } from './motion.js';

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
// How far behind the pull each module arrives, as a share of the whole pull: a cascade from the top left.
const DELAY = { 'cc-scenes': 0, 'cc-media': 0.06, 'cc-bright': 0.12, 'cc-volume': 0.16, 'cc-heat': 0.2, 'cc-quiet': 0.17, 'cc-lock': 0.21, 'cc-lights': 0.25, 'cc-timer': 0.29 };

export function createControlCenter(ctx) {
  const { screen, settings } = ctx;
  const q = (s) => screen.querySelector(s);
  const layer = q('#cc');
  const progress = new Motion(0, { spring: SPRINGS.release, epsilon: 0.001 });
  const tracker = velocityTracker();
  const modules = Object.entries(DELAY).map(([id, delay]) => {
    const el = q(`#${id}`);
    const m = { id, el, g: ctx.glass(el), delay, k: 0, scale: 1 };
    m.g.geometry = () => {
      const b = m.g.base;
      return { x: b.x + (b.w * (1 - m.scale)) / 2, y: b.y + (b.h * (1 - m.scale)) / 2, w: b.w * m.scale, h: b.h * m.scale, r: m.g.r * m.scale };
    };
    return m;
  });
  const byId = Object.fromEntries(modules.map((m) => [m.id, m]));
  let drag = null;
  let shownTrack = '';
  // Closed, the layer is hidden, and hidden elements measure as nothing: show it while the panel measures.
  let hiddenBefore = true;
  ctx.onMeasure({
    before() { hiddenBefore = layer.hidden; layer.hidden = false; },
    after() { layer.hidden = hiddenBefore; pickSet = false; },
  });

  function settle(to, velocity = 0) {
    if (to > 0) layer.hidden = false;
    progress.to(to, { spring: SPRINGS.release, velocity });
    ctx.setAppOpen(to > 0 || (ctx.appProgress ?? 0) > 0.5);
  }

  // One pull handler serves the top edge (to open) and the open layer (to push it away).
  const pull = {
    quiet: true,
    begin() {
      drag = { p0: progress.value };
      progress.set(progress.value);
      layer.hidden = false;
      tracker.reset();
      ctx.setAppOpen(true);
    },
    move(dy, t) {
      const raw = drag.p0 + dy / (380 * ctx.unit);
      const p = raw > 1 ? 1 + rubberBand(raw - 1, 1) * 0.12 : Math.max(0, raw);
      tracker.add(t, p);
      progress.set(p, tracker.velocity(t));
    },
    end(t) {
      const v = tracker.velocity(t);
      drag = null;
      settle(progress.value + project(v, 0.99) > 0.5 ? 1 : 0, v);
    },
  };
  ctx.addDragTarget(q('#cc-edge'), pull, 'y');
  ctx.addDragTarget(layer, pull, 'y');
  layer.addEventListener('click', (e) => { if (e.target === layer) settle(0); });
  window.addEventListener('keydown', (e) => { if (e.key === 'Escape' && progress.target === 1) settle(0); });

  // ---------- sliders: glass capsules with a fill inside that tracks the finger 1:1 ----------
  const sliders = [
    { id: 'cc-bright', motion: () => ctx.home.levels.living, toF: (v) => v, fromF: (f) => f },
    { id: 'cc-volume', motion: () => ctx.home.volume, toF: (v) => v, fromF: (f) => f },
    { id: 'cc-heat', motion: () => ctx.home.setpoint, toF: (v) => (v - 16) / 12, fromF: (f) => 16 + f * 12, step: 0.5 },
  ];
  for (const s of sliders) {
    const m = byId[s.id];
    const fill = ctx.surface(m.el.querySelector('.cc-fill'));
    fill.clipEl = null;
    fill.alpha = () => clamp(m.k, 0, 1);
    fill.geometry = () => {
      const g = m.g.geometry();
      const f = clamp(s.toF(s.motion().value), 0, 1.06);
      const h = Math.max(g.w, g.h * f);
      return { x: g.x, y: g.y + m.g.dy + g.h - h, w: g.w, h, r: g.w / 2 };
    };
    const track = velocityTracker();
    let f0 = 0;
    ctx.addDragTarget(m.el, {
      begin() { f0 = s.toF(s.motion().value); track.reset(); },
      move(dy, t) {
        const raw = f0 - dy / m.g.base.h;
        const f = raw < 0 ? -rubberBand(-raw, 1) * 0.1 : raw > 1 ? 1 + rubberBand(raw - 1, 1) * 0.1 : raw;
        track.add(t, s.fromF(f));
        s.motion().set(s.fromF(f), track.velocity(t));
      },
      end() {
        const v = clamp(s.motion().value, s.fromF(0), s.fromF(1));
        const to = s.step ? Math.round(v / s.step) * s.step : v;
        if (to !== s.motion().value) s.motion().to(to, { spring: s.step ? SPRINGS.detent : SPRINGS.release, velocity: 0 });
      },
    }, 'y');
    // a press on the capsule sets it there, on the snap-to-target spring
    m.el.addEventListener('click', (e) => {
      const r = m.el.getBoundingClientRect();
      const f = clamp(1 - (e.clientY - r.top) / r.height, 0, 1);
      const v = s.fromF(f);
      s.motion().to(s.step ? Math.round(v / s.step) * s.step : v, { spring: SPRINGS.settle });
    });
  }

  // ---------- scenes: the chosen one sits on a signal disc that glides between them ----------
  const pickX = new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 });
  const pickY = new Motion(0, { spring: SPRINGS.release, epsilon: 0.05 });
  const scenes = [...layer.querySelectorAll('[data-cc-scene]')];
  let scene = 'coffee';
  let pickSet = false;
  const pick = ctx.surface(q('#cc-pick'));
  pick.alpha = () => clamp(byId['cc-scenes'].k, 0, 1);
  pick.geometry = () => {
    const g = byId['cc-scenes'].g.geometry();
    const size = 64 * ctx.unit * byId['cc-scenes'].scale;
    return { x: g.x + pickX.value * byId['cc-scenes'].scale, y: g.y + byId['cc-scenes'].g.dy + pickY.value * byId['cc-scenes'].scale, w: size, h: size, r: size / 2 };
  };
  function place(animate) {
    const button = scenes.find((b) => b.dataset.ccScene === scene);
    const module = byId['cc-scenes'].el.getBoundingClientRect();
    const r = button.getBoundingClientRect();
    const s = byId['cc-scenes'].scale || 1;
    const x = (r.left - module.left) / s;
    const y = (r.top - module.top) / s;
    if (animate) { pickX.to(x); pickY.to(y); } else { pickX.set(x); pickY.set(y); }
    for (const b of scenes) b.classList.toggle('is-on', b === button);
  }
  for (const b of scenes) {
    b.addEventListener('click', () => {
      scene = b.dataset.ccScene;
      ctx.applyScene?.(scene);
      place(true);
    });
  }

  // ---------- media and toggles ----------
  for (const b of layer.querySelectorAll('[data-cc-skip]')) b.addEventListener('click', () => ctx.skipTrack?.(Number(b.dataset.ccSkip)));
  q('#cc-play').addEventListener('click', () => ctx.togglePlay?.());
  const toggles = {
    'cc-quiet': () => { settings.quiet = !settings.quiet; },
    'cc-lock': () => ctx.security?.toggle(),
    'cc-lights': () => q('#all-off')?.click(),
    'cc-timer': () => ctx.timers?.add(300),
  };
  for (const [id, act] of Object.entries(toggles)) byId[id].el.addEventListener('click', act);

  Object.defineProperty(ctx, 'ccProgress', { get: () => (layer.hidden ? 0 : clamp(progress.value, 0, 1)), configurable: true });
  ctx.setControlCenter(() => (layer.hidden ? null : { progress: clamp(progress.value, 0, 1), dim: clamp(progress.value, 0, 1) * 0.9, glass: 1 }));

  ctx.onFrame(() => {
    const p = progress.value;
    if (!layer.hidden && !drag && progress.target === 0 && p < 0.002 && !progress.moving) {
      layer.hidden = true;
      pickSet = false;
    }
    if (layer.hidden) return {};
    const u = ctx.unit;
    for (const m of modules) {
      m.k = clamp((p - m.delay) / (1 - m.delay), 0, 1.2);
      const shown = Math.min(m.k, 1);
      m.scale = settings.calm ? 1 : 0.86 + 0.14 * m.k;
      m.g.dy = settings.calm ? 0 : (1 - shown) * -52 * u;
      m.g.shift = `scale(${m.scale.toFixed(4)}) `;
      m.g.strength.set(shown);
      m.el.style.opacity = clamp(shown * 1.4, 0, 1).toFixed(3);
    }
    if (!pickSet) { place(false); pickSet = true; }
    // what the modules show follows the house, wherever it was changed
    const np = ctx.nowPlaying?.();
    if (np && `${np.title}${np.playing}` !== shownTrack) {
      shownTrack = `${np.title}${np.playing}`;
      q('#cc-title').textContent = np.title;
      q('#cc-by').textContent = np.by;
      q('#cc-play .icon').textContent = np.playing ? 'pause' : 'play_arrow';
    }
    q('#cc-heat-value').textContent = `${(Math.round(ctx.home.setpoint.value * 2) / 2).toFixed(1)}°`;
    const locked = ctx.security?.locked ?? true;
    byId['cc-lock'].el.setAttribute('aria-pressed', String(locked));
    byId['cc-lock'].el.querySelector('.icon').textContent = locked ? 'lock' : 'lock_open';
    byId['cc-lock'].g.tint.set(locked ? 0 : 1);
    byId['cc-quiet'].el.setAttribute('aria-pressed', String(!!settings.quiet));
    byId['cc-quiet'].g.tint.set(settings.quiet ? 1 : 0);
    return {};
  });

  return { open: () => settle(1), close: () => settle(0) };
}
